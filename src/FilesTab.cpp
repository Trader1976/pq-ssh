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
#include <QDirIterator>
#include <QSet>


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

struct UploadTask {
    QString localPath;
    QString remotePath;
    quint64 size = 0;
};

static QString joinRemote(const QString& base, const QString& name)
{
    if (base.endsWith('/')) return base + name;
    return base + "/" + name;
}

static void collectDirRecursive(const QString& localRoot,
                                const QString& remoteRoot,
                                QVector<UploadTask>& out)
{
    QDirIterator it(localRoot,
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();

        // Skip symlinks for safety (avoid loops / weird targets)
        if (fi.isSymLink())
            continue;

        if (fi.isFile()) {
            const QString rel = QDir(localRoot).relativeFilePath(fi.absoluteFilePath());
            UploadTask t;
            t.localPath  = fi.absoluteFilePath();
            t.remotePath = joinRemote(remoteRoot, rel);
            t.size       = (quint64)fi.size();
            out.push_back(t);
        }
    }
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
    m_uploadFolderBtn = new QPushButton("Upload folder…", top); // NEW
    m_downloadBtn = new QPushButton("Download…", top);
    m_localUpBtn = new QPushButton("Local Up", top);
    m_upBtn       = new QPushButton("Remote Up", top);

    m_remotePathLabel = new QLabel("Remote: ~", top);
    m_remotePathLabel->setStyleSheet("color:#888;");
    topL->addWidget(m_localUpBtn);
    topL->addWidget(m_upBtn);
    topL->addWidget(m_refreshBtn);
    topL->addWidget(m_uploadBtn);
    topL->addWidget(m_uploadFolderBtn);
    topL->addWidget(m_downloadBtn);
    topL->addWidget(m_remotePathLabel, 1);

    // Splitter with local + remote
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    // Local
    m_localModel = new QFileSystemModel(this);
    m_localModel->setRootPath(QDir::homePath());
    m_localCwd = QDir::homePath();

    m_localView = new QTreeView(split);
    m_localView->setModel(m_localModel);
    m_localView->setRootIndex(m_localModel->index(QDir::homePath()));
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSortingEnabled(true);
    m_localView->sortByColumn(0, Qt::AscendingOrder);
    // Track local directory navigation (double-click folders)
    m_localCwd = QDir::homePath();

    connect(m_localView, &QTreeView::doubleClicked, this,
            [this](const QModelIndex& idx) {
                if (!m_localModel) return;

                const QFileInfo fi = m_localModel->fileInfo(idx);
                if (!fi.isDir()) return;

                const QString path = fi.absoluteFilePath();
                m_localCwd = path;
                m_localView->setRootIndex(m_localModel->index(path));
            });

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
    connect(m_localUpBtn, &QPushButton::clicked, this, &FilesTab::goLocalUp);
    connect(m_upBtn, &QPushButton::clicked, this, &FilesTab::goRemoteUp);
    connect(m_uploadBtn, &QPushButton::clicked, this, &FilesTab::uploadSelected);
    connect(m_downloadBtn, &QPushButton::clicked, this, &FilesTab::downloadSelected);
    connect(m_uploadFolderBtn, &QPushButton::clicked, this, &FilesTab::uploadFolder);
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
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select local files to upload", QDir::homePath(), "All files (*)");

    startUploadPaths(files);
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
    startUploadPaths(localPaths);
}

void FilesTab::startUploadPaths(const QStringList& paths)
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Upload", "Not connected.");
        return;
    }
    if (paths.isEmpty()) return;

    // Build task list (files + folders)
    QVector<UploadTask> tasks;
    tasks.reserve(256);

    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
            // Preserve folder name under current remote cwd
            const QString remoteRoot = joinRemote(m_remoteCwd, fi.fileName());
            collectDirRecursive(fi.absoluteFilePath(), remoteRoot, tasks);
        } else if (fi.isFile()) {
            UploadTask t;
            t.localPath  = fi.absoluteFilePath();
            t.remotePath = joinRemote(m_remoteCwd, fi.fileName());
            t.size       = (quint64)fi.size();
            tasks.push_back(t);
        }
    }

    if (tasks.isEmpty()) {
        QMessageBox::information(this, "Upload", "No files found to upload.");
        return;
    }

    // Compute total bytes for a real progress bar across all files
    quint64 totalBytes = 0;
    for (const auto& t : tasks) totalBytes += t.size;

    // Create needed remote directories (best-effort, de-dup)
    QSet<QString> dirs;
    dirs.reserve(tasks.size());

    for (const auto& t : tasks) {
        const QString dir = QFileInfo(t.remotePath).path();
        dirs.insert(dir);
    }

    runTransfer(QString("Uploading %1 items…").arg(tasks.size()),
                [this, tasks, totalBytes, dirs](QString *err) -> bool {

                    // 1) mkdir remote dirs
                    for (const QString& d : dirs) {
                        QString e;
                        if (!m_ssh->ensureRemoteDir(d, 0755, &e)) {
                            if (err) *err = "Failed to create remote dir:\n" + d + "\n" + e;
                            return false;
                        }
                    }

                    // 2) upload sequentially with aggregated progress
                    quint64 completedBytes = 0;

                    for (int i = 0; i < tasks.size(); ++i) {
                        const auto& t = tasks[i];

                        quint64 lastFileDone = 0;

                        QString e;
                        const bool ok = m_ssh->uploadFile(
                            t.localPath,
                            t.remotePath,
                            &e,
                            [this, &completedBytes, &lastFileDone, totalBytes](quint64 fileDone, quint64 /*fileTotal*/) {

                                // Aggregate: overall = completedBytes + delta(fileDone)
                                const quint64 delta = (fileDone >= lastFileDone)
                                    ? (fileDone - lastFileDone)
                                    : 0;

                                lastFileDone = fileDone;

                                const quint64 overallDone = completedBytes + delta;

                                QMetaObject::invokeMethod(this, "onTransferProgress",
                                    Qt::QueuedConnection,
                                    Q_ARG(quint64, overallDone),
                                    Q_ARG(quint64, totalBytes));
                            });

                        if (!ok) {
                            if (err) *err = e;
                            return false; // includes cancel path
                        }

                        completedBytes += t.size;
                    }

                    return true;
                });
}

void FilesTab::uploadFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Select folder to upload", QDir::homePath());

    if (dir.isEmpty()) return;

    startUploadPaths(QStringList{dir});
}
void FilesTab::goLocalUp()
{
    if (!m_localModel || !m_localView) return;

    QString cur = m_localCwd;
    if (cur.isEmpty()) {
        cur = m_localModel->filePath(m_localView->rootIndex());
    }

    QDir d(cur);
    if (!d.cdUp()) return; // already at filesystem root

    const QString up = d.absolutePath();
    m_localCwd = up;
    m_localView->setRootIndex(m_localModel->index(up));
}
