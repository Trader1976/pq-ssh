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
#include <QDateTime>
#include <QDirIterator>
#include <QSet>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QMenu>
#include <QAction>
#include <QInputDialog>

static QString prettySize(quint64 bytes)
{
    const double b = (double)bytes;
    if (b < 1024.0) return QString("%1 B").arg(bytes);
    if (b < 1024.0*1024.0) return QString::number(b/1024.0, 'f', 1) + " KB";
    if (b < 1024.0*1024.0*1024.0) return QString::number(b/(1024.0*1024.0), 'f', 1) + " MB";
    return QString::number(b/(1024.0*1024.0*1024.0), 'f', 1) + " GB";
}

struct UploadTask {
    QString localPath;
    QString remotePath;
    quint64 size = 0;
};

static QString joinRemote(const QString& base, const QString& rel)
{
    if (base.endsWith('/')) return base + rel;
    return base + "/" + rel;
}

static QString shQuote(const QString& s)
{
    QString out = s;
    out.replace("'", "'\"'\"'");
    return "'" + out + "'";
}

static QString joinLocal(const QString& base, const QString& rel)
{
    return QDir(base).filePath(rel);
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

    m_localUpBtn = new QPushButton("Local Up", top);
    m_remoteUpBtn = new QPushButton("Remote Up", top);
    m_refreshBtn = new QPushButton("Refresh", top);
    m_uploadBtn = new QPushButton("Upload…", top);
    m_uploadFolderBtn = new QPushButton("Upload folder…", top);
    m_downloadBtn = new QPushButton("Download…", top);

    m_remotePathLabel = new QLabel("Remote: ~", top);
    m_remotePathLabel->setStyleSheet("color:#888;");

    topL->addWidget(m_localUpBtn);
    topL->addWidget(m_remoteUpBtn);
    topL->addWidget(m_refreshBtn);
    topL->addWidget(m_uploadBtn);
    topL->addWidget(m_uploadFolderBtn);
    topL->addWidget(m_downloadBtn);
    topL->addWidget(m_remotePathLabel, 1);

    // Splitter
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    // Local
    m_localModel = new QFileSystemModel(this);
    m_localModel->setRootPath(QDir::homePath());
    m_localCwd = QDir::homePath();

    m_localView = new QTreeView(split);
    m_localView->setModel(m_localModel);
    m_localView->setRootIndex(m_localModel->index(m_localCwd));
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSortingEnabled(true);
    m_localView->sortByColumn(0, Qt::AscendingOrder);
    m_localView->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Double-click folders to navigate local
    connect(m_localView, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (!m_localModel) return;
        const QFileInfo fi = m_localModel->fileInfo(idx);
        if (!fi.isDir()) return;
        m_localCwd = fi.absoluteFilePath();
        m_localView->setRootIndex(m_localModel->index(m_localCwd));
    });

    // Drag files from local tree
    m_localView->setDragEnabled(true);
    m_localView->setDragDropMode(QAbstractItemView::DragOnly);
    m_localView->setDefaultDropAction(Qt::CopyAction);
    m_localView->setAutoScroll(true);

    // ALSO accept drops from remote table (custom mime) -> triggers download
    m_localView->viewport()->setAcceptDrops(true);
    m_localView->viewport()->installEventFilter(this);

    m_localView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_localView, &QWidget::customContextMenuRequested,
            this, &FilesTab::showLocalContextMenu);

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

    m_remoteTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_remoteTable, &QWidget::customContextMenuRequested,
            this, &FilesTab::showRemoteContextMenu);

    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);

    outer->addWidget(top);
    outer->addWidget(split, 1);

    connect(m_refreshBtn, &QPushButton::clicked, this, &FilesTab::refreshRemote);
    connect(m_localUpBtn, &QPushButton::clicked, this, &FilesTab::goLocalUp);
    connect(m_remoteUpBtn, &QPushButton::clicked, this, &FilesTab::goRemoteUp);
    connect(m_uploadBtn, &QPushButton::clicked, this, &FilesTab::uploadSelected);
    connect(m_uploadFolderBtn, &QPushButton::clicked, this, &FilesTab::uploadFolder);
    connect(m_downloadBtn, &QPushButton::clicked, this, &FilesTab::downloadSelected);
    connect(m_remoteTable, &QTableWidget::cellDoubleClicked, this, &FilesTab::remoteItemActivated);
    connect(m_remoteTable, &RemoteDropTable::filesDropped, this, &FilesTab::onRemoteFilesDropped);
}

bool FilesTab::eventFilter(QObject *obj, QEvent *event)
{
    // Catch remote->local drops onto local tree viewport
    if (m_localView && obj == m_localView->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *e = static_cast<QDragMoveEvent*>(event);
            const QMimeData *md = e->mimeData();
            if (md && md->hasFormat("application/x-pqssh-remote-paths")) {
                e->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            auto *e = static_cast<QDropEvent*>(event);
            const QMimeData *md = e->mimeData();
            if (md && md->hasFormat("application/x-pqssh-remote-paths")) {
                const QString payload = QString::fromUtf8(md->data("application/x-pqssh-remote-paths"));
                const QStringList remotePaths = payload.split('\n', Qt::SkipEmptyParts);

                if (!remotePaths.isEmpty()) {
                    // Download into current local cwd (root of view)
                    startDownloadPaths(remotePaths, m_localCwd);
                    e->acceptProposedAction();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void FilesTab::setRemoteCwd(const QString& path)
{
    m_remoteCwd = path;
    if (m_remotePathLabel)
        m_remotePathLabel->setText("Remote: " + m_remoteCwd);
}

QString FilesTab::remoteParentDir(const QString& p) const
{
    QString s = p.trimmed();
    if (s.isEmpty() || s == "/" || s == "~") return s;

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
    setRemoteCwd(!pwd.isEmpty() ? pwd : QStringLiteral("~"));

    refreshRemote();
}

void FilesTab::onSshDisconnected()
{
    if (m_remoteTable)
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
            if (e.mtime > 0)
                mtimeStr = QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy-MM-dd HH:mm");

            auto *mtItem = new QTableWidgetItem(mtimeStr);

            m_remoteTable->setItem(i, 0, nameItem);
            m_remoteTable->setItem(i, 1, typeItem);
            m_remoteTable->setItem(i, 2, sizeItem);
            m_remoteTable->setItem(i, 3, mtItem);
        }
    });

    watcher->setFuture(QtConcurrent::run([this, path]() -> QVector<SshClient::RemoteEntry> {
        QVector<SshClient::RemoteEntry> out;
        QString e;
        if (!m_ssh->listRemoteDir(path, &out, &e)) out.clear();
        return out;
    }));
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

void FilesTab::goLocalUp()
{
    if (!m_localModel || !m_localView) return;

    QString cur = m_localCwd;
    if (cur.isEmpty())
        cur = m_localModel->filePath(m_localView->rootIndex());

    QDir d(cur);
    if (!d.cdUp()) return;

    m_localCwd = d.absolutePath();
    m_localView->setRootIndex(m_localModel->index(m_localCwd));
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

    connect(m_progressDlg, &QProgressDialog::canceled, this, [this]() {
        if (m_ssh) m_ssh->requestCancelTransfer();
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
            QMessageBox::warning(this, "Transfer", "Transfer failed or cancelled.");
        } else {
            refreshRemote();
        }
    });

    watcher->setFuture(QtConcurrent::run([fn]() -> bool {
        QString err;
        return fn(&err);
    }));
}

void FilesTab::uploadSelected()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select local files to upload", m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd, "All files (*)");

    startUploadPaths(files);
}

void FilesTab::uploadFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Select folder to upload", m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd);

    if (dir.isEmpty()) return;
    startUploadPaths(QStringList{dir});
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

    QVector<UploadTask> tasks;
    tasks.reserve(256);

    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
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

    quint64 totalBytes = 0;
    for (const auto& t : tasks) totalBytes += t.size;

    QSet<QString> dirs;
    dirs.reserve(tasks.size());
    for (const auto& t : tasks) {
        dirs.insert(QFileInfo(t.remotePath).path());
    }

    runTransfer(QString("Uploading %1 item(s)…").arg(tasks.size()),
                [this, tasks, totalBytes, dirs](QString *err) -> bool {

        // 1) ensure dirs
        for (const QString& d : dirs) {
            QString e;
            if (!m_ssh->ensureRemoteDir(d, 0755, &e)) {
                if (err) *err = "Failed to create remote dir:\n" + d + "\n" + e;
                return false;
            }
        }

        // 2) upload sequentially + aggregated progress
        quint64 completedBytes = 0;

        for (const auto& t : tasks) {
            quint64 lastFileDone = 0;
            QString e;

            const bool ok = m_ssh->uploadFile(
                t.localPath,
                t.remotePath,
                &e,
                [this, &completedBytes, &lastFileDone, totalBytes](quint64 fileDone, quint64 /*fileTotal*/) {

                    const quint64 delta = (fileDone >= lastFileDone) ? (fileDone - lastFileDone) : 0;
                    lastFileDone = fileDone;

                    QMetaObject::invokeMethod(this, "onTransferProgress",
                        Qt::QueuedConnection,
                        Q_ARG(quint64, completedBytes + delta),
                        Q_ARG(quint64, totalBytes));
                });

            if (!ok) {
                if (err) *err = e;
                return false;
            }

            completedBytes += t.size;
        }

        return true;
    });
}

bool FilesTab::collectRemoteRecursive(const QString& remoteRoot,
                                      const QString& localRoot,
                                      QVector<QString>& outRemoteFiles,
                                      QVector<QString>& outLocalFiles,
                                      QVector<quint64>& outSizes,
                                      quint64* totalBytes,
                                      QString* err)
{
    QVector<SshClient::RemoteEntry> items;
    QString e;
    if (!m_ssh->listRemoteDir(remoteRoot, &items, &e)) {
        if (err) *err = e;
        return false;
    }

    for (const auto& it : items) {
        const QString remoteFull = it.fullPath;
        const QString localFull  = joinLocal(localRoot, it.name);

        if (it.isDir) {
            QDir().mkpath(localFull);
            if (!collectRemoteRecursive(remoteFull, localFull,
                                        outRemoteFiles, outLocalFiles, outSizes,
                                        totalBytes, err)) {
                return false;
            }
        } else {
            outRemoteFiles.push_back(remoteFull);
            outLocalFiles.push_back(localFull);
            outSizes.push_back(it.size);
            if (totalBytes) *totalBytes += it.size;
        }
    }

    return true;
}

void FilesTab::startDownloadPaths(const QStringList& remotePaths, const QString& destDir)
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, "Download", "Not connected.");
        return;
    }
    if (remotePaths.isEmpty()) return;

    QVector<QString> rem;
    QVector<QString> loc;
    QVector<quint64> sizes;
    rem.reserve(512);
    loc.reserve(512);
    sizes.reserve(512);

    quint64 totalBytes = 0;

    for (const QString& rp : remotePaths) {
        SshClient::RemoteEntry info;
        QString stErr;
        if (!m_ssh->statRemotePath(rp, &info, &stErr)) {
            QMessageBox::warning(this, "Download", "Cannot stat remote path:\n" + rp + "\n" + stErr);
            return;
        }

        const QString name = info.name.isEmpty() ? QFileInfo(rp).fileName() : info.name;
        const QString localRoot = joinLocal(destDir, name);

        if (info.isDir) {
            QDir().mkpath(localRoot);
            QString e;
            if (!collectRemoteRecursive(rp, localRoot, rem, loc, sizes, &totalBytes, &e)) {
                QMessageBox::warning(this, "Download", e);
                return;
            }
        } else {
            rem.push_back(rp);
            loc.push_back(localRoot);
            sizes.push_back(info.size);
            totalBytes += info.size;

            // Ensure local dir exists
            QDir().mkpath(QFileInfo(localRoot).absolutePath());
        }
    }

    runTransfer(QString("Downloading %1 file(s)…").arg(rem.size()),
                [this, rem, loc, sizes, totalBytes](QString *err) -> bool {

        quint64 completedBytes = 0;

        for (int i = 0; i < rem.size(); ++i) {
            quint64 last = 0;

            const bool ok = m_ssh->downloadFile(
                rem[i],
                loc[i],
                err,
                [this, &completedBytes, &last, totalBytes](quint64 d, quint64 /*t*/) {

                    const quint64 delta = (d >= last) ? (d - last) : 0;
                    last = d;

                    QMetaObject::invokeMethod(this, "onTransferProgress",
                        Qt::QueuedConnection,
                        Q_ARG(quint64, completedBytes + delta),
                        Q_ARG(quint64, totalBytes));
                });

            if (!ok)
                return false;

            completedBytes += sizes[i];
        }

        return true;
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
        QMessageBox::information(this, "Download", "Select file(s) or folder(s) first.");
        return;
    }

    const QString destDir = QFileDialog::getExistingDirectory(
        this, "Select local destination folder", m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd);
    if (destDir.isEmpty()) return;

    QStringList remotePaths;
    for (const auto& idx : rows) {
        auto *it = m_remoteTable->item(idx.row(), 0);
        if (it) remotePaths << it->data(Qt::UserRole).toString();
    }

    startDownloadPaths(remotePaths, destDir);
}

void FilesTab::showLocalContextMenu(const QPoint& pos)
{
    if (!m_localView || !m_localModel) return;

    QMenu menu(this);
    QAction* del = menu.addAction("Delete");

    const QAction* chosen = menu.exec(m_localView->viewport()->mapToGlobal(pos));
    if (chosen == del) {
        deleteLocalSelection();
    }
}

void FilesTab::showRemoteContextMenu(const QPoint& pos)
{
    if (!m_remoteTable) return;

    QMenu menu(this);
    QAction* del = menu.addAction("Delete");

    const QAction* chosen = menu.exec(m_remoteTable->viewport()->mapToGlobal(pos));
    if (chosen == del) {
        deleteRemoteSelection();
    }
}

void FilesTab::deleteLocalSelection()
{
    if (!m_localView || !m_localModel) return;

    const QModelIndexList rows = m_localView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Delete", "Select one or more local items first.");
        return;
    }

    QStringList paths;
    paths.reserve(rows.size());
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = m_localModel->fileInfo(idx);
        if (fi.exists())
            paths << fi.absoluteFilePath();
    }
    paths.removeDuplicates();

    if (paths.isEmpty()) return;

    const QString msg = QString("Delete %1 local item(s)?\n\nThis cannot be undone.")
                            .arg(paths.size());

    if (QMessageBox::question(this, "Delete", msg,
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
                             }

    // Delete (files + folders)
    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
            QDir dir(p);
            if (!dir.removeRecursively()) {
                QMessageBox::warning(this, "Delete",
                                     "Failed to delete folder:\n" + p);
                return;
            }
        } else {
            if (!QFile::remove(p)) {
                QMessageBox::warning(this, "Delete",
                                     "Failed to delete file:\n" + p);
                return;
            }
        }
    }

    // Refresh local view (keep same cwd)
    m_localView->setRootIndex(m_localModel->index(m_localCwd));
}

void FilesTab::deleteRemoteSelection()
{
    if (!m_ssh || !m_ssh->isConnected() || !m_remoteTable) {
        QMessageBox::information(this, "Delete", "Not connected.");
        return;
    }

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Delete", "Select one or more remote items first.");
        return;
    }

    struct Item { QString path; bool isDir = false; };
    QVector<Item> items;
    items.reserve(rows.size());

    for (const auto& idx : rows) {
        auto* it = m_remoteTable->item(idx.row(), 0);
        if (!it) continue;

        Item x;
        x.path  = it->data(Qt::UserRole).toString();
        x.isDir = (it->data(Qt::UserRole + 1).toInt() == 1);
        if (!x.path.isEmpty())
            items.push_back(x);
    }

    if (items.isEmpty()) return;

    const QString msg =
        QString("Delete %1 remote item(s)?\n\nFolders will be deleted recursively.")
            .arg(items.size());

    if (QMessageBox::question(this, "Delete", msg,
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
                             }

    runTransfer(QString("Deleting %1 item(s)…").arg(items.size()),
                [this, items](QString* err) -> bool {

        for (const auto& x : items) {
            // Use "--" to avoid option injection. Quote everything.
            const QString cmd = x.isDir
                ? QString("rm -rf -- %1").arg(shQuote(x.path))
                : QString("rm -f  -- %1").arg(shQuote(x.path));

            QString out, e;
            if (!m_ssh->exec(cmd, &out, &e)) {
                if (err) *err = "Remote delete failed:\n" + x.path + "\n" + e;
                return false;
            }
        }
        return true;
    });
}
