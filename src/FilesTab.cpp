#include "FilesTab.h"
#include "RemoteDropTable.h"
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QTreeView>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QFileInfo>
#include <QDir>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QDateTime>


static QString prettySize(quint64 bytes)
{
    const double b = (double)bytes;
    if (b < 1024.0) return QString("%1 B").arg(bytes);
    if (b < 1024.0*1024.0) return QString::number(b/1024.0, 'f', 1) + " KB";
    if (b < 1024.0*1024.0*1024.0) return QString::number(b/(1024.0*1024.0), 'f', 1) + " MB";
    return QString::number(b/(1024.0*1024.0*1024.0), 'f', 1) + " GB";
}

FilesTab::FilesTab(SshClient *ssh, QWidget *parent)
    : QWidget(parent), m_ssh(ssh)
{
    buildUi();
    setRemoteCwd(QStringLiteral("~")); // placeholder until connected
}

void FilesTab::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);

    // Top bar
    auto *top = new QWidget(this);
    auto *topL = new QHBoxLayout(top);
    topL->setContentsMargins(0, 0, 0, 0);
    topL->setSpacing(6);

    m_upBtn = new QPushButton("Up", top);
    m_refreshBtn = new QPushButton("Refresh", top);
    m_uploadBtn = new QPushButton("Upload…", top);
    m_downloadBtn = new QPushButton("Download…", top);

    m_remotePathLabel = new QLabel("Remote: ~", top);
    m_remotePathLabel->setStyleSheet("color:#888;");

    topL->addWidget(m_upBtn);
    topL->addWidget(m_refreshBtn);
    topL->addWidget(m_uploadBtn);
    topL->addWidget(m_downloadBtn);
    topL->addWidget(m_remotePathLabel, 1);

    // Splitter with local + remote
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    // Local
    m_localModel = new QFileSystemModel(this);
    m_localModel->setRootPath(QDir::homePath());

    m_localView = new QTreeView(split);
    m_localView->setModel(m_localModel);
    m_localView->setRootIndex(m_localModel->index(QDir::homePath()));
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSortingEnabled(true);
    m_localView->sortByColumn(0, Qt::AscendingOrder);

    // Enable dragging files from local tree
    m_localView->setDragEnabled(true);
    m_localView->setDragDropMode(QAbstractItemView::DragOnly);
    m_localView->setDefaultDropAction(Qt::CopyAction);
    m_localView->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_localView->setDragDropOverwriteMode(false);
    m_localView->setAutoScroll(true);

    // Remote
    m_remoteTable = new RemoteDropTable(split);
    m_remoteTable->setColumnCount(4);
    m_remoteTable->setHorizontalHeaderLabels({"Name", "Type", "Size", "Modified"});
    m_remoteTable->horizontalHeader()->setStretchLastSection(true);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_remoteTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTable->setShowGrid(false);
    m_remoteTable->setAlternatingRowColors(true);

    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);

    outer->addWidget(top);
    outer->addWidget(split, 1);

    connect(m_refreshBtn, &QPushButton::clicked, this, &FilesTab::refreshRemote);
    connect(m_upBtn, &QPushButton::clicked, this, &FilesTab::goRemoteUp);
    connect(m_uploadBtn, &QPushButton::clicked, this, &FilesTab::uploadSelected);
    connect(m_downloadBtn, &QPushButton::clicked, this, &FilesTab::downloadSelected);
    connect(m_remoteTable, &QTableWidget::cellDoubleClicked, this, &FilesTab::remoteItemActivated);
    connect(m_remoteTable, &RemoteDropTable::filesDropped,
            this, &FilesTab::onRemoteFilesDropped);


}

void FilesTab::setRemoteCwd(const QString& path)
{
    m_remoteCwd = path;
    m_remotePathLabel->setText("Remote: " + m_remoteCwd);
}

QString FilesTab::remoteParentDir(const QString& p) const
{
    QString s = p.trimmed();
    if (s.isEmpty() || s == "/" || s == "~") return s;

    // Strip trailing slash (except root)
    if (s.endsWith('/') && s != "/") s.chop(1);

    const int idx = s.lastIndexOf('/');
    if (idx <= 0) return "/";

    return s.left(idx);
}

void FilesTab::onSshConnected()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        setRemoteCwd("~");
        return;
    }

    QString e;
    const QString pwd = m_ssh->remotePwd(&e);
    if (!pwd.isEmpty()) setRemoteCwd(pwd);
    else setRemoteCwd("~");

    refreshRemote();
}

void FilesTab::onSshDisconnected()
{
    m_remoteTable->setRowCount(0);
    setRemoteCwd("~");
}

void FilesTab::refreshRemote()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Files", "Not connected.");
        return;
    }

    const QString path = m_remoteCwd;

    // run listing in background (avoid UI freeze)
    auto *watcher = new QFutureWatcher<QVector<SshClient::RemoteEntry>>(this);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const QVector<SshClient::RemoteEntry> items = watcher->result();
        watcher->deleteLater();

        m_remoteTable->setRowCount(0);
        m_remoteTable->setRowCount(items.size());

        for (int i = 0; i < items.size(); ++i) {
            const auto &e = items[i];

            auto *nameItem = new QTableWidgetItem(e.name);
            nameItem->setData(Qt::UserRole, e.fullPath);
            nameItem->setData(Qt::UserRole + 1, e.isDir ? 1 : 0);

            auto *typeItem = new QTableWidgetItem(e.isDir ? "DIR" : "FILE");
            auto *sizeItem = new QTableWidgetItem(e.isDir ? "" : prettySize(e.size));

            QString mtimeStr;
            if (e.mtime > 0) {
                mtimeStr = QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy-MM-dd HH:mm");
            }
            auto *mtItem = new QTableWidgetItem(mtimeStr);

            m_remoteTable->setItem(i, 0, nameItem);
            m_remoteTable->setItem(i, 1, typeItem);
            m_remoteTable->setItem(i, 2, sizeItem);
            m_remoteTable->setItem(i, 3, mtItem);
        }
    });

    QFuture<QVector<SshClient::RemoteEntry>> fut = QtConcurrent::run([this, path]() -> QVector<SshClient::RemoteEntry> {
        QVector<SshClient::RemoteEntry> out;
        QString e;
        if (!m_ssh->listRemoteDir(path, &out, &e)) {
            // keep silent here; UI thread will show empty list.
            out.clear();
        }
        return out;
    });

    watcher->setFuture(fut);
}

void FilesTab::goRemoteUp()
{
    if (!m_ssh || !m_ssh->isConnected()) return;

    const QString up = remoteParentDir(m_remoteCwd);
    if (!up.isEmpty() && up != m_remoteCwd) {
        setRemoteCwd(up);
        refreshRemote();
    }
}

void FilesTab::remoteItemActivated(int row, int /*col*/)
{
    auto *it = m_remoteTable->item(row, 0);
    if (!it) return;

    const QString full = it->data(Qt::UserRole).toString();
    const bool isDir = it->data(Qt::UserRole + 1).toInt() == 1;

    if (isDir) {
        setRemoteCwd(full);
        refreshRemote();
    }
}

void FilesTab::onTransferProgress(quint64 done, quint64 total)
{
    if (!m_progressDlg) return;

    // Best effort: if total unknown -> show “busy” style
    if (total == 0) {
        m_progressDlg->setMaximum(0);
        m_progressDlg->setValue(0);
        return;
    }

    m_progressDlg->setMaximum((int)std::min<quint64>(total, (quint64)INT_MAX));
    m_progressDlg->setValue((int)std::min<quint64>(done,  (quint64)INT_MAX));
}

void FilesTab::runTransfer(const QString& title,
                           const std::function<bool(QString *err)> &fn)
{
    if (m_progressDlg) {
        m_progressDlg->deleteLater();
        m_progressDlg = nullptr;
    }

    m_progressDlg = new QProgressDialog(title, "Cancel", 0, 100, this);
    m_progressDlg->setWindowModality(Qt::WindowModal);
    m_progressDlg->setMinimumDuration(200);
    m_progressDlg->setAutoClose(true);
    m_progressDlg->setAutoReset(true);
    m_progressDlg->setValue(0);

    // MVP: cancel button only cancels the dialog (transfer continues).
    // (We can add real cancel later by adding a cancellation flag to SshClient loops.)
    connect(m_progressDlg, &QProgressDialog::canceled, this, [this]() {
        if (m_ssh)
            m_ssh->requestCancelTransfer();
    });

    auto *watcher = new QFutureWatcher<bool>(this);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const bool ok = watcher->result();
        watcher->deleteLater();

        if (m_progressDlg) {
            m_progressDlg->setValue(m_progressDlg->maximum());
            m_progressDlg->deleteLater();
            m_progressDlg = nullptr;
        }

        if (!ok) {
            QMessageBox::warning(this, "Transfer", "Transfer failed. See log / retry.");
        } else {
            refreshRemote();
        }
    });

    QFuture<bool> fut = QtConcurrent::run([fn]() -> bool {
        QString err;
        const bool ok = fn(&err);
        return ok;
    });

    watcher->setFuture(fut);
}

void FilesTab::uploadSelected()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Upload", "Not connected.");
        return;
    }

    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select local files to upload", QDir::homePath(), "All files (*)");
    if (files.isEmpty()) return;

    // MVP: upload sequentially; show progress for the first file only
    const QString first = files.first();
    const QString remote = (m_remoteCwd.endsWith('/') ? m_remoteCwd : m_remoteCwd + "/")
        + QFileInfo(first).fileName();

    runTransfer(QString("Uploading %1…").arg(QFileInfo(first).fileName()),
                [this, first, remote](QString *err) -> bool {
                    return m_ssh->uploadFile(first, remote, err,
                        [this](quint64 d, quint64 t) {
                            QMetaObject::invokeMethod(this, "onTransferProgress",
                                Qt::QueuedConnection,
                                Q_ARG(quint64, d),
                                Q_ARG(quint64, t));
                        });
                });
}

void FilesTab::downloadSelected()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Download", "Not connected.");
        return;
    }

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Download", "Select one or more remote files first.");
        return;
    }

    const QString destDir = QFileDialog::getExistingDirectory(
        this, "Select local destination folder", QDir::homePath());
    if (destDir.isEmpty()) return;

    // MVP: download first selected row with progress (add multi later)
    const int r = rows.first().row();
    auto *it = m_remoteTable->item(r, 0);
    if (!it) return;

    const bool isDir = it->data(Qt::UserRole + 1).toInt() == 1;
    if (isDir) {
        QMessageBox::information(this, "Download", "Folder download not implemented in MVP.");
        return;
    }

    const QString remote = it->data(Qt::UserRole).toString();
    const QString local  = QDir(destDir).filePath(QFileInfo(remote).fileName());

    runTransfer(QString("Downloading %1…").arg(QFileInfo(remote).fileName()),
                [this, remote, local](QString *err) -> bool {
                    return m_ssh->downloadFile(remote, local, err,
                        [this](quint64 d, quint64 t) {
                            QMetaObject::invokeMethod(this, "onTransferProgress",
                                Qt::QueuedConnection,
                                Q_ARG(quint64, d),
                                Q_ARG(quint64, t));
                        });
                });
}
void FilesTab::onRemoteFilesDropped(const QStringList& localPaths)
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Upload", "Not connected.");
        return;
    }

    // Filter to real files only (skip directories for MVP)
    QStringList files;
    for (const QString& p : localPaths) {
        QFileInfo fi(p);
        if (fi.exists() && fi.isFile())
            files << fi.absoluteFilePath();
    }

    if (files.isEmpty()) {
        QMessageBox::information(this, "Upload", "Drop contained no regular files (folders not supported yet).");
        return;
    }

    const QString cwd = m_remoteCwd; // capture

    runTransfer(QString("Uploading %1 file(s)…").arg(files.size()),
                [this, files, cwd](QString *err) -> bool {

                    for (int i = 0; i < files.size(); ++i) {
                        const QString local = files[i];
                        const QString remote =
                            (cwd.endsWith('/') ? cwd : cwd + "/") + QFileInfo(local).fileName();

                        // Progress callback updates UI thread
                        auto progress = [this](quint64 d, quint64 t) {
                            QMetaObject::invokeMethod(this, "onTransferProgress",
                                Qt::QueuedConnection,
                                Q_ARG(quint64, d),
                                Q_ARG(quint64, t));
                        };

                        QString e;
                        if (!m_ssh->uploadFile(local, remote, &e, progress)) {
                            if (err) *err = QString("Upload failed for '%1' → '%2': %3").arg(local, remote, e);
                            return false;
                        }
                    }

                    return true;
                });
}
