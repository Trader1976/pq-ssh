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
#include <QEvent>
#include <QStyle>
#include <algorithm>
#include <QGuiApplication>
#include <QClipboard>
#include <QInputDialog>


// -----------------------------------------------------------------------------
// joinListPreview()
// -----------------------------------------------------------------------------
// Utility for logs/UI messages: show a short preview of a string list.
// Keeps logs readable when many files are selected.
// -----------------------------------------------------------------------------
static QString joinListPreview(const QStringList& xs, int maxN = 6)
{
    QStringList out;
    for (int i = 0; i < xs.size() && i < maxN; ++i) out << xs[i];
    if (xs.size() > maxN) out << QString("…(+%1)").arg(xs.size() - maxN);
    return out.join(", ");
}


// -----------------------------------------------------------------------------
// prettySize()
// -----------------------------------------------------------------------------
// Human-readable file size formatting (B / KB / MB / GB).
// Used only for display, not for sorting (sorting uses raw size roles).
// -----------------------------------------------------------------------------
static QString prettySize(quint64 bytes)
{
    const double b = (double)bytes;
    if (b < 1024.0) return QString("%1 B").arg(bytes);
    if (b < 1024.0 * 1024.0) return QString::number(b / 1024.0, 'f', 1) + " KB";
    if (b < 1024.0 * 1024.0 * 1024.0) return QString::number(b / (1024.0 * 1024.0), 'f', 1) + " MB";
    return QString::number(b / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
}

// -----------------------------------------------------------------------------
// joinRemote()
// -----------------------------------------------------------------------------
// Join remote paths using forward slashes.
// NOTE: remote paths are treated as POSIX-like (even on Windows client).
// -----------------------------------------------------------------------------
static QString joinRemote(const QString& base, const QString& rel)
{
    if (base.endsWith('/')) return base + rel;
    return base + "/" + rel;
}

// -----------------------------------------------------------------------------
// joinLocal()
// -----------------------------------------------------------------------------
// Join local paths using QDir::filePath (platform-correct separators).
// -----------------------------------------------------------------------------
static QString joinLocal(const QString& base, const QString& rel)
{
    return QDir(base).filePath(rel);
}

// -----------------------------------------------------------------------------
// shQuote()
// -----------------------------------------------------------------------------
// Minimal POSIX shell quoting for remote exec commands (mv/rm/mkdir).
// Prevents whitespace and basic injection issues by wrapping in single quotes
// and escaping embedded single quotes via '"'"' sequence.
// -----------------------------------------------------------------------------
static QString shQuote(const QString& s)
{
    QString out = s;
    out.replace("'", "'\"'\"'");
    return "'" + out + "'";
}

// -----------------------------------------------------------------------------
// Custom item roles for remote table sorting and semantics.
//
// We store "real" sortable values in roles so UI text formatting doesn't break
// ordering. RoleSortGroup enforces:
//   ".." first (-1), then directories (0), then files (1)
// across ALL columns.
// -----------------------------------------------------------------------------
static constexpr int RoleIsParent  = Qt::UserRole + 100;
static constexpr int RoleSortGroup = Qt::UserRole + 101; // -1 parent, 0 dir, 1 file
static constexpr int RoleSortName  = Qt::UserRole + 102; // lowercase name
static constexpr int RoleSortSize  = Qt::UserRole + 103; // quint64
static constexpr int RoleSortMtime = Qt::UserRole + 104; // qint64





// -----------------------------------------------------------------------------
// RemoteSortItem
// -----------------------------------------------------------------------------
// Custom QTableWidgetItem that sorts with directory-first semantics for the
// remote file table, regardless of the currently sorted column.
//
// Sorting policy (always):
//   1) Parent row ".." first
//   2) Directories next
//   3) Files last
// Then do column-specific comparison.
// -----------------------------------------------------------------------------
class RemoteSortItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem& other) const override
    {
        const int col = column();

        // Always keep ".." first, then dirs, then files (for ALL columns)
        const int g1 = data(RoleSortGroup).toInt();
        const int g2 = other.data(RoleSortGroup).toInt();
        if (g1 != g2) return g1 < g2; // -1 < 0 < 1

        // Column-specific compare
        if (col == 2) { // Size
            const quint64 s1 = data(RoleSortSize).toULongLong();
            const quint64 s2 = other.data(RoleSortSize).toULongLong();
            if (s1 != s2) return s1 < s2;
        } else if (col == 3) { // Modified
            const qint64 t1 = data(RoleSortMtime).toLongLong();
            const qint64 t2 = other.data(RoleSortMtime).toLongLong();
            if (t1 != t2) return t1 < t2;
        } else {
            // Name / Type: compare by lowercase name for stability
            const QString n1 = data(RoleSortName).toString();
            const QString n2 = other.data(RoleSortName).toString();
            if (n1 != n2) return n1 < n2;
        }

        // Final fallback: Qt default text compare
        return QTableWidgetItem::operator<(other);
    }
};


// -----------------------------------------------------------------------------
// collectDirRecursive()
// -----------------------------------------------------------------------------
// Expand a local directory selection into a flat list of upload tasks.
//
// - localRoot: absolute local directory path chosen by the user
// - remoteRoot: remote directory base where the directory will be mirrored
// - out: receives one UploadTask per local file (directories are implied)
//
// Notes:
// - Symbolic links are skipped to avoid surprising uploads and recursion loops.
// - Uses QDirIterator::Subdirectories for convenience.
// -----------------------------------------------------------------------------
static void collectDirRecursive(const QString& localRoot,
                                const QString& remoteRoot,
                                QVector<FilesTab::UploadTask>& out)
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

            FilesTab::UploadTask t;
            t.localPath  = fi.absoluteFilePath();
            t.remotePath = joinRemote(remoteRoot, rel);
            t.size       = (quint64)fi.size();
            out.push_back(t);
        }
    }
}

// -----------------------------------------------------------------------------
// FilesTab ctor
// -----------------------------------------------------------------------------
// UI tab for the file transfer UI:
// - Local tree view (QFileSystemModel)
// - Remote table (via libssh SFTP through SshClient)
// - Drag&drop support (local->remote and remote->local)
// -----------------------------------------------------------------------------
FilesTab::FilesTab(SshClient *ssh, QWidget *parent)
    : QWidget(parent), m_ssh(ssh)
{
    buildUi();

    // Start with a safe default until SSH connection tells us real pwd.
    setRemoteCwd(QStringLiteral("~"));

    // Local side starts at home by default.
    m_localCwd = QDir::homePath();
}

// -----------------------------------------------------------------------------
// buildUi()
// -----------------------------------------------------------------------------
// Constructs UI widgets and wiring:
// - top toolbar buttons
// - local tree view (drag source, drop target for remote paths)
// - remote table (drop target for local files, drag source for remote paths)
// -----------------------------------------------------------------------------
void FilesTab::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);


    // =========================
    // Top bar
    // =========================
    auto *top = new QWidget(this);
    auto *topL = new QHBoxLayout(top);
    topL->setContentsMargins(0, 0, 0, 0);
    topL->setSpacing(6);

    m_localUpBtn        = new QPushButton(tr("Local Up"), top);
    m_remoteUpBtn       = new QPushButton(tr("Remote Up"), top);
    m_refreshBtn        = new QPushButton(tr("Refresh"), top);
    m_uploadBtn         = new QPushButton(tr("Upload…"), top);
    m_uploadFolderBtn   = new QPushButton(tr("Upload folder…"), top);
    m_downloadBtn       = new QPushButton(tr("Download…"), top);

    // Remote path indicator (informational; does not accept editing here)
    m_remotePathLabel   = new QLabel(tr("Remote: ~"), top);
    m_remotePathLabel->setStyleSheet("color:#888;");

    topL->addWidget(m_localUpBtn);
    topL->addWidget(m_remoteUpBtn);
    topL->addWidget(m_refreshBtn);
    topL->addWidget(m_uploadBtn);
    topL->addWidget(m_uploadFolderBtn);
    topL->addWidget(m_downloadBtn);
    topL->addWidget(m_remotePathLabel, 1);

    // =========================
    // Splitter
    // =========================
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    // =========================
    // Local side
    // =========================
    // QFileSystemModel gives us a lightweight file browser with sorting.
    m_localModel = new QFileSystemModel(this);
    m_localModel->setRootPath(QDir::homePath());

    m_localView = new QTreeView(split);
    m_localView->setModel(m_localModel);
    m_localView->setRootIndex(m_localModel->index(m_localCwd));
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_localView->setSortingEnabled(true);
    m_localView->sortByColumn(0, Qt::AscendingOrder);

    // 🔹 Make local "Name" column wider (UI preference)
    m_localView->setColumnWidth(0, 110);

    // Local: swap Type and Size columns (Name=0, Size=1, Type=2, Modified=3)
    auto *lh = m_localView->header();
    lh->setSectionsMovable(true);

    // Move "Type" (2) to where "Size" (1) is
    lh->moveSection(2, 1);

    // Double-click directory to enter it (local navigation)
    connect(m_localView, &QTreeView::doubleClicked, this,
            [this](const QModelIndex& idx) {
        if (!m_localModel) return;
        const QFileInfo fi = m_localModel->fileInfo(idx);
        if (!fi.isDir()) return;
        m_localCwd = fi.absoluteFilePath();
        m_localView->setRootIndex(m_localModel->index(m_localCwd));
    });

    // Drag from local → remote (we only initiate drags from local)
    m_localView->setDragEnabled(true);
    m_localView->setDragDropMode(QAbstractItemView::DragOnly);
    m_localView->setDefaultDropAction(Qt::CopyAction);
    m_localView->setAutoScroll(true);

    // Accept drops from remote (remote->local download)
    // We install an event filter so we can handle a custom mime type.
    m_localView->viewport()->setAcceptDrops(true);
    m_localView->viewport()->installEventFilter(this);

    // Local context menu
    m_localView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_localView, &QWidget::customContextMenuRequested,
            this, &FilesTab::showLocalContextMenu);

    // =========================
    // Remote side
    // =========================
    // RemoteDropTable supports:
    // - dropping local file URLs (local->remote upload)
    // - dragging remote rows out using custom mime type (remote->local download)
    m_remoteTable = new RemoteDropTable(split);

    m_remoteTable->setColumnCount(4);
    m_remoteTable->setHorizontalHeaderLabels(QStringList{
        tr("Name"),
        tr("Type"),
        tr("Size"),
        tr("Modified")
    });

    auto *hdr = m_remoteTable->horizontalHeader();
    hdr->setSectionResizeMode(QHeaderView::Interactive); // user-resizable
    hdr->setStretchLastSection(true);
    hdr->setSectionsClickable(true);
    hdr->setSortIndicatorShown(true);

    // Initial widths (user can resize)
    m_remoteTable->setColumnWidth(0, 140); // Name
    m_remoteTable->setColumnWidth(1, 80);  // Type
    m_remoteTable->setColumnWidth(2, 90);  // Size
    m_remoteTable->setColumnWidth(3, 150); // Modified

    m_remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTable->setShowGrid(false);
    m_remoteTable->setAlternatingRowColors(true);

    // Enable sorting (actual ordering is enforced via RemoteSortItem roles)
    m_remoteTable->setSortingEnabled(true);

    // Unified file font (match local + remote)
    QFont fileFont = font();
    fileFont.setPointSize(11);
    m_localView->setFont(fileFont);
    m_remoteTable->setFont(fileFont);
    m_remoteTable->horizontalHeader()->setFont(fileFont);

    // Tighter rows
    m_localView->setUniformRowHeights(true);
    m_remoteTable->verticalHeader()->setDefaultSectionSize(22);

    // Remote context menu
    m_remoteTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_remoteTable, &QWidget::customContextMenuRequested,
            this, &FilesTab::showRemoteContextMenu);

    // =========================
    // Layout + wiring
    // =========================
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

    // Remote navigation by double-click (dir enters, ".." goes up)
    connect(m_remoteTable, &QTableWidget::cellDoubleClicked,
            this, &FilesTab::remoteItemActivated);

    // Local file(s) dropped onto remote table → upload
    connect(m_remoteTable, &RemoteDropTable::filesDropped,
            this, &FilesTab::onRemoteFilesDropped);
}


// -----------------------------------------------------------------------------
// eventFilter()
// -----------------------------------------------------------------------------
// Handles remote->local drag&drop onto the local tree viewport.
//
// RemoteDropTable encodes selected remote paths into a custom mime type:
//   "application/x-pqssh-remote-paths"
// payload is newline-separated paths.
//
// When such a payload is dropped on the local tree, we initiate download
// into current local working directory (m_localCwd).
// -----------------------------------------------------------------------------
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
                    startDownloadPaths(remotePaths, m_localCwd);
                    e->acceptProposedAction();
                    return true;
                }
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

// -----------------------------------------------------------------------------
// setRemoteCwd()
// -----------------------------------------------------------------------------
// Updates current remote working directory and refreshes the status label.
// The actual listing refresh is triggered separately (refreshRemote()).
// -----------------------------------------------------------------------------
void FilesTab::setRemoteCwd(const QString& path)
{
    m_remoteCwd = path;
    if (m_remotePathLabel)
        m_remotePathLabel->setText(
            tr("Remote: %1").arg(m_remoteCwd)
        );
}

// -----------------------------------------------------------------------------
// remoteParentDir()
// -----------------------------------------------------------------------------
// Returns the parent directory for a remote path.
// Handles "~" and "/" safely.
// -----------------------------------------------------------------------------
QString FilesTab::remoteParentDir(const QString& p) const
{
    QString s = p.trimmed();
    if (s.isEmpty() || s == "/" || s == "~") return s;

    if (s.endsWith('/') && s != "/") s.chop(1);

    const int idx = s.lastIndexOf('/');
    if (idx <= 0) return "/";

    return s.left(idx);
}

// -----------------------------------------------------------------------------
// onSshConnected()
// -----------------------------------------------------------------------------
// Called by the app when SSH connects successfully.
// We query remote `pwd` (via libssh exec) and refresh listing.
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// onSshDisconnected()
// -----------------------------------------------------------------------------
// Called when SSH disconnects. Clears remote listing and resets label.
// -----------------------------------------------------------------------------
void FilesTab::onSshDisconnected()
{
    if (m_remoteTable)
        m_remoteTable->setRowCount(0);
    setRemoteCwd("~");
}

// -----------------------------------------------------------------------------
// refreshRemote()
// -----------------------------------------------------------------------------
// Fetch remote directory listing via SshClient::listRemoteDir and rebuild table.
// Uses RemoteSortItem + role metadata to enforce:
//   ".." first, directories next, files last.
// -----------------------------------------------------------------------------
void FilesTab::refreshRemote()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, tr("Files"), tr("Not connected."));
        return;
    }

    QVector<SshClient::RemoteEntry> items;
    QString err;
    if (!m_ssh->listRemoteDir(m_remoteCwd, &items, &err)) {
        QMessageBox::warning(this, "Remote", err);
        return;
    }

    // Rebuild table safely (disable sorting during insertion)
    m_remoteTable->setSortingEnabled(false);
    m_remoteTable->clearContents();
    m_remoteTable->setRowCount(0);

    // Icons (platform-native via QStyle)
    const QIcon dirIcon  = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    const QIcon upIcon   = style()->standardIcon(QStyle::SP_FileDialogToParent);

    // Optional: add parent row (..)
    const bool showParent = !(m_remoteCwd == "/" || m_remoteCwd == "~");
    int row = 0;

    if (showParent) {
        m_remoteTable->insertRow(row);

        // Parent row has no real path, is treated specially by RoleIsParent.
        auto *nameItem = new RemoteSortItem("..");
        nameItem->setIcon(upIcon);
        nameItem->setData(RoleIsParent, true);
        nameItem->setData(Qt::UserRole, QString());          // no fullPath needed
        nameItem->setData(Qt::UserRole + 1, 1);              // treat like dir for UI logic
        nameItem->setData(RoleSortGroup, -1);
        nameItem->setData(RoleSortName, QString(""));        // always first anyway

        auto *typeItem = new RemoteSortItem("DIR");
        typeItem->setData(RoleSortGroup, -1);

        auto *sizeItem = new RemoteSortItem("");
        sizeItem->setData(RoleSortGroup, -1);
        sizeItem->setData(RoleSortSize, (qulonglong)0);

        auto *mtItem = new RemoteSortItem("");
        mtItem->setData(RoleSortGroup, -1);
        mtItem->setData(RoleSortMtime, (qlonglong)0);

        m_remoteTable->setItem(row, 0, nameItem);
        m_remoteTable->setItem(row, 1, typeItem);
        m_remoteTable->setItem(row, 2, sizeItem);
        m_remoteTable->setItem(row, 3, mtItem);

        row++;
    }

    // Normal rows
    for (const auto& e : items) {
        m_remoteTable->insertRow(row);

        // Column 0 stores fullPath in Qt::UserRole (also used by RemoteDropTable drag-out)
        auto *nameItem = new RemoteSortItem(e.name);
        nameItem->setData(Qt::UserRole, e.fullPath);
        nameItem->setData(Qt::UserRole + 1, e.isDir ? 1 : 0);
        nameItem->setIcon(e.isDir ? dirIcon : fileIcon);

        // Sorting roles
        const int group = e.isDir ? 0 : 1;
        nameItem->setData(RoleSortGroup, group);
        nameItem->setData(RoleSortName, e.name.toLower());

        auto *typeItem = new RemoteSortItem(e.isDir ? "DIR" : "FILE");
        typeItem->setData(RoleSortGroup, group);

        auto *sizeItem = new RemoteSortItem(e.isDir ? "" : prettySize(e.size));
        sizeItem->setData(RoleSortGroup, group);
        sizeItem->setData(RoleSortSize, (qulonglong)e.size);

        // Convert remote mtime (epoch seconds) into local display time
        QString mtimeStr;
        if (e.mtime > 0)
            mtimeStr = QDateTime::fromSecsSinceEpoch(e.mtime).toString("yyyy-MM-dd HH:mm");

        auto *mtItem = new RemoteSortItem(mtimeStr);
        mtItem->setData(RoleSortGroup, group);
        mtItem->setData(RoleSortMtime, (qlonglong)e.mtime);

        m_remoteTable->setItem(row, 0, nameItem);
        m_remoteTable->setItem(row, 1, typeItem);
        m_remoteTable->setItem(row, 2, sizeItem);
        m_remoteTable->setItem(row, 3, mtItem);

        row++;
    }

    // Re-enable sorting and apply default sort by Name ascending
    m_remoteTable->setSortingEnabled(true);
    m_remoteTable->sortByColumn(0, Qt::AscendingOrder);
}




// -----------------------------------------------------------------------------
// goRemoteUp()
// -----------------------------------------------------------------------------
// Navigate to remote parent directory and refresh listing.
// -----------------------------------------------------------------------------
void FilesTab::goRemoteUp()
{
    if (!m_ssh || !m_ssh->isConnected()) return;

    const QString up = remoteParentDir(m_remoteCwd);
    if (!up.isEmpty() && up != m_remoteCwd) {
        setRemoteCwd(up);
        refreshRemote();
    }
}

// -----------------------------------------------------------------------------
// goLocalUp()
// -----------------------------------------------------------------------------
// Navigate local tree root to parent directory.
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// remoteItemActivated()
// -----------------------------------------------------------------------------
// Handle remote table double-click:
// - ".." row navigates up
// - directory row navigates into directory
// - files do nothing on double-click (download is explicit)
// -----------------------------------------------------------------------------
void FilesTab::remoteItemActivated(int row, int /*col*/)
{
    auto *it = m_remoteTable->item(row, 0);
    if (!it) return;

    // Special parent row ("..") -> go up
    if (it->data(RoleIsParent).toBool()) {
        goRemoteUp();
        return;
    }

    const QString full = it->data(Qt::UserRole).toString();
    const bool isDir = it->data(Qt::UserRole + 1).toInt() == 1;

    if (isDir) {
        setRemoteCwd(full);
        refreshRemote();
    }
}

// -----------------------------------------------------------------------------
// onTransferProgress()
// -----------------------------------------------------------------------------
// UI thread progress handler (called via queued invoke from worker thread).
// Uses INT_MAX caps because QProgressDialog uses int.
// -----------------------------------------------------------------------------
void FilesTab::onTransferProgress(quint64 done, quint64 total)
{
    if (!m_progressDlg) return;

    if (total == 0) {
        m_progressDlg->setMaximum(0);
        m_progressDlg->setValue(0);
        return;
    }

    m_progressDlg->setMaximum((int)std::min<quint64>(total, (quint64)INT_MAX));
    m_progressDlg->setValue((int)std::min<quint64>(done, (quint64)INT_MAX));
}

// -----------------------------------------------------------------------------
// runTransfer()
// -----------------------------------------------------------------------------
// Run a potentially long transfer task in a background thread (QtConcurrent),
// show a modal progress dialog, and allow cancellation.
//
// Cancellation model:
//   - Progress dialog Cancel triggers SshClient::requestCancelTransfer()
//   - SshClient checks m_cancelRequested in its streaming loops (best-effort)
//
// On completion:
//   - show error message if failed
//   - refresh remote listing if succeeded
// -----------------------------------------------------------------------------
void FilesTab::runTransfer(const QString& title,
                           const std::function<bool(QString *err)> &fn)
{
    struct TransferResult {
        bool ok = false;
        QString err;
    };

    if (m_progressDlg) {
        m_progressDlg->deleteLater();
        m_progressDlg = nullptr;
    }

    m_progressDlg = new QProgressDialog(title, tr("Cancel"), 0, 100, this);
    m_progressDlg->setWindowModality(Qt::WindowModal);
    m_progressDlg->setMinimumDuration(200);
    m_progressDlg->setAutoClose(true);
    m_progressDlg->setAutoReset(true);
    m_progressDlg->setValue(0);

    connect(m_progressDlg, &QProgressDialog::canceled, this, [this]() {
        if (m_ssh) m_ssh->requestCancelTransfer();
    });

    auto *watcher = new QFutureWatcher<TransferResult>(this);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const TransferResult r = watcher->result();
        watcher->deleteLater();

        if (m_progressDlg) {
            m_progressDlg->setValue(m_progressDlg->maximum());
            m_progressDlg->deleteLater();
            m_progressDlg = nullptr;
        }

        if (!r.ok) {
            const QString msg = r.err.trimmed().isEmpty()
                ? tr("Transfer failed or cancelled.")
                : r.err.trimmed();

            // Developer log (do NOT translate)
            qWarning().noquote()
                << QString("[FILES][TRANSFER] FAILED: %1").arg(msg);

            // User-facing dialog (translated title)
            QMessageBox::warning(
                this,
                tr("Transfer"),
                msg
            );
        } else {
            qInfo().noquote() << "[FILES][TRANSFER] OK";
            refreshRemote();
        }
    });

    watcher->setFuture(QtConcurrent::run([fn]() -> TransferResult {
        TransferResult r;
        QString err;
        r.ok = fn(&err);
        r.err = err;
        return r;
    }));
}


// -----------------------------------------------------------------------------
// uploadSelected()
// -----------------------------------------------------------------------------
// Explicit upload action: shows file picker and uploads selected file(s)
// into current remote directory (m_remoteCwd).
// -----------------------------------------------------------------------------
void FilesTab::uploadSelected()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Select local files to upload"),
        m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd,
        tr("All files (*)"));

    startUploadPaths(files);
}

// -----------------------------------------------------------------------------
// uploadFolder()
// -----------------------------------------------------------------------------
// Explicit folder upload: user selects a directory, we recursively scan it and
// mirror it to the remote side.
// -----------------------------------------------------------------------------
void FilesTab::uploadFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select folder to upload"),
        m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd);

    if (dir.isEmpty()) return;
    startUploadPaths(QStringList{dir});
}

// -----------------------------------------------------------------------------
// onRemoteFilesDropped()
// -----------------------------------------------------------------------------
// Triggered when local files are dropped onto RemoteDropTable (local->remote).
// -----------------------------------------------------------------------------
void FilesTab::onRemoteFilesDropped(const QStringList& localPaths)
{
    startUploadPaths(localPaths);
}

// -----------------------------------------------------------------------------
// startUploadPaths()
// -----------------------------------------------------------------------------
// Plan + execute upload of a mixed selection (files and folders):
//   1) Build a flat list of UploadTask
//   2) Ensure all remote directories exist
//   3) Upload files sequentially with aggregated progress
//   4) Verify SHA-256 for each file (integrity check)
//
// Notes:
// - Uses runTransfer() to avoid UI blocking.
// - Uses SshClient streaming uploadFile() with cancel support.
// -----------------------------------------------------------------------------
void FilesTab::startUploadPaths(const QStringList& paths)
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, tr("Upload"), tr("Not connected."));
        return;
    }
    if (paths.isEmpty()) return;

    // Logs: do NOT translate (keep stable for debugging / parsing)
    qInfo().noquote() << QString("[XFER][UPLOAD] request paths=%1 remoteCwd='%2'")
                         .arg(paths.size())
                         .arg(m_remoteCwd);
    qInfo().noquote() << QString("[XFER][UPLOAD] paths: %1").arg(joinListPreview(paths));

    QVector<UploadTask> tasks;
    tasks.reserve(256);

    // Expand selection into file-level tasks
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
            // Mirror directory under remote cwd using directory name as root
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
        qWarning().noquote() << "[XFER][UPLOAD] no tasks after scanning selection";
        QMessageBox::information(this, tr("Upload"), tr("No files found to upload."));
        return;
    }

    quint64 totalBytes = 0;
    for (const auto& t : tasks) totalBytes += t.size;

    qInfo().noquote() << QString("[XFER][UPLOAD] planned files=%1 total=%2")
                         .arg(tasks.size())
                         .arg(prettySize(totalBytes));

    // Pre-compute unique remote directories to create
    QSet<QString> dirs;
    dirs.reserve(tasks.size());
    for (const auto& t : tasks)
        dirs.insert(QFileInfo(t.remotePath).path());

    runTransfer(tr("Uploading %1 item(s)…").arg(tasks.size()),
                [this, tasks, totalBytes, dirs](QString *err) -> bool {

        qInfo().noquote() << QString("[XFER][UPLOAD] batch start files=%1 total=%2 dirs=%3")
                             .arg(tasks.size())
                             .arg(prettySize(totalBytes))
                             .arg(dirs.size());

        // 1) ensure dirs exist
        for (const QString& d : dirs) {
            QString e;
            if (!m_ssh->ensureRemoteDir(d, 0755, &e)) {
                // Developer log (do NOT translate)
                qWarning().noquote()
                    << QString("[XFER][UPLOAD] ensureRemoteDir FAIL '%1' : %2")
                           .arg(d, e);

                // User-facing error (translate)
                if (err) {
                    *err = tr("Failed to create remote directory:\n%1\n%2")
                               .arg(d, e);
                }
                return false;
            }
        }

        // 2) upload sequentially + aggregated progress
        quint64 completedBytes = 0;

        for (const auto& t : tasks) {
            quint64 lastFileDone = 0;
            QString e;

            qInfo().noquote() << QString("[XFER][UPLOAD] start %1 -> %2 (%3)")
                                 .arg(t.localPath, t.remotePath, prettySize(t.size));

            const bool ok = m_ssh->uploadFile(
                t.localPath,
                t.remotePath,
                &e,
                [this, &completedBytes, &lastFileDone, totalBytes](quint64 fileDone, quint64 /*fileTotal*/) {

                    // Convert per-file progress into total batch progress
                    const quint64 delta = (fileDone >= lastFileDone) ? (fileDone - lastFileDone) : 0;
                    lastFileDone = fileDone;

                    QMetaObject::invokeMethod(this, "onTransferProgress",
                        Qt::QueuedConnection,
                        Q_ARG(quint64, completedBytes + delta),
                        Q_ARG(quint64, totalBytes));
                });

            if (!ok) {
                qWarning().noquote() << QString("[XFER][UPLOAD] FAIL %1 -> %2 : %3")
                                        .arg(t.localPath, t.remotePath, e);

                if (err) {
                    *err = tr("Upload failed:\n%1\n→ %2\n\n%3")
                               .arg(t.localPath, t.remotePath, e);
                }
                return false;
            }

            completedBytes += t.size;

            // Integrity check (currently always on; later can be gated by settings)
            {
                QString verr;
                if (!m_ssh->verifyLocalVsRemoteSha256(t.localPath, t.remotePath, &verr)) {
                    qWarning().noquote() << QString("[XFER][UPLOAD] verify FAIL %1 : %2")
                                            .arg(t.remotePath, verr);
                    if (err) {
                        *err = tr("Integrity check failed for upload:\n%1\n\n%2")
                                   .arg(t.remotePath, verr);
                    }
                    return false;
                }
                qInfo().noquote() << QString("[XFER][UPLOAD] verify OK %1").arg(t.remotePath);
            }
        }

        qInfo().noquote() << QString("[XFER][UPLOAD] batch OK files=%1 total=%2")
                             .arg(tasks.size())
                             .arg(prettySize(totalBytes));

        return true;
    });
}


// -----------------------------------------------------------------------------
// collectRemoteRecursive()
// -----------------------------------------------------------------------------
// Expand a remote directory into a flat list of files to download, while also
// creating local directory structure.
//
// Output vectors are parallel arrays:
//   outRemoteFiles[i] -> outLocalFiles[i] with outSizes[i]
//
// totalBytes accumulates sum of file sizes for progress bar.
// -----------------------------------------------------------------------------
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
            // Mirror directory locally before descending
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

            // Ensure parent dir exists for file path
            QDir().mkpath(QFileInfo(localFull).absolutePath());
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// startDownloadPaths()
// -----------------------------------------------------------------------------
// Plan + execute download of a mixed selection (files and folders):
//   1) Stat each selection item to detect dir/file
//   2) Expand dirs recursively into a flat file list
//   3) Download sequentially with aggregated progress
//   4) Verify SHA-256 for each file (integrity check)
//
// Destination:
//   destDir is a local directory chosen by user or implied by drag-drop.
// -----------------------------------------------------------------------------
void FilesTab::startDownloadPaths(const QStringList& remotePaths, const QString& destDir)
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, tr("Download"), tr("Not connected."));
        return;
    }
    if (remotePaths.isEmpty()) return;

    qInfo().noquote() << QString("[XFER][DOWNLOAD] request paths=%1 destDir='%2'")
                         .arg(remotePaths.size())
                         .arg(destDir);
    qInfo().noquote() << QString("[XFER][DOWNLOAD] paths: %1").arg(joinListPreview(remotePaths));

    QVector<QString> rem;
    QVector<QString> loc;
    QVector<quint64> sizes;
    rem.reserve(512);
    loc.reserve(512);
    sizes.reserve(512);

    quint64 totalBytes = 0;

    // Expand selection (files + folders) into flat file list
    for (const QString& rp : remotePaths) {
        SshClient::RemoteEntry info;
        QString stErr;
        if (!m_ssh->statRemotePath(rp, &info, &stErr)) {
            qWarning().noquote() << QString("[XFER][DOWNLOAD] stat FAIL '%1' : %2").arg(rp, stErr);
            QMessageBox::warning(
                this,
                tr("Download"),
                tr("Cannot stat remote path:\n%1\n%2")
                    .arg(rp, stErr)
            );
            return;
        }

        // Name for local root: prefer stat name, fallback to path filename
        const QString name = !info.name.isEmpty() ? info.name : QFileInfo(rp).fileName();
        const QString localRoot = joinLocal(destDir, name);

        if (info.isDir) {
            qInfo().noquote() << QString("[XFER][DOWNLOAD] expand dir '%1' -> '%2'").arg(rp, localRoot);

            QDir().mkpath(localRoot);

            QString e;
            if (!collectRemoteRecursive(rp, localRoot, rem, loc, sizes, &totalBytes, &e)) {
                qWarning().noquote() << QString("[XFER][DOWNLOAD] expand FAIL '%1' : %2").arg(rp, e);
                QMessageBox::warning(
                    this,
                    tr("Download"),
                    e
                );
                return;
            }
        } else {
            rem.push_back(rp);
            loc.push_back(localRoot);
            sizes.push_back(info.size);
            totalBytes += info.size;

            QDir().mkpath(QFileInfo(localRoot).absolutePath());

            qInfo().noquote() << QString("[XFER][DOWNLOAD] plan file '%1' -> '%2' (%3)")
                                 .arg(rp, localRoot, prettySize(info.size));
        }
    }

    qInfo().noquote() << QString("[XFER][DOWNLOAD] planned files=%1 total=%2")
                         .arg(rem.size())
                         .arg(prettySize(totalBytes));

    runTransfer(tr("Downloading %1 file(s)…").arg(rem.size()),
                [this, rem, loc, sizes, totalBytes](QString *err) -> bool {

        qInfo().noquote() << QString("[XFER][DOWNLOAD] batch start files=%1 total=%2")
                             .arg(rem.size())
                             .arg(prettySize(totalBytes));

        quint64 completedBytes = 0;

        for (int i = 0; i < rem.size(); ++i) {
            quint64 last = 0;

            qInfo().noquote() << QString("[XFER][DOWNLOAD] start %1 -> %2 (%3)")
                                 .arg(rem[i], loc[i], prettySize(sizes[i]));

            QString e;
            const bool ok = m_ssh->downloadFile(
                rem[i],
                loc[i],
                &e,
                [this, &completedBytes, &last, totalBytes](quint64 d, quint64 /*t*/) {

                    // Convert per-file progress into total batch progress
                    const quint64 delta = (d >= last) ? (d - last) : 0;
                    last = d;

                    QMetaObject::invokeMethod(this, "onTransferProgress",
                        Qt::QueuedConnection,
                        Q_ARG(quint64, completedBytes + delta),
                        Q_ARG(quint64, totalBytes));
                });

            if (!ok) {
                qWarning().noquote() << QString("[XFER][DOWNLOAD] FAIL %1 -> %2 : %3")
                                        .arg(rem[i], loc[i], e);
                if (err) *err = e;
                return false;
            }

            completedBytes += sizes[i];

            // Integrity check (currently always on; later can be gated by settings)
            {
                QString verr;
                if (!m_ssh->verifyRemoteVsLocalSha256(rem[i], loc[i], &verr)) {
                    qWarning().noquote() << QString("[XFER][DOWNLOAD] verify FAIL %1 : %2")
                                            .arg(loc[i], verr);
                    if (err) {
                        *err = tr("Integrity check failed for download:\n%1\n\n%2")
                                   .arg(loc[i], verr);
                    }
                    return false;
                }
                qInfo().noquote() << QString("[XFER][DOWNLOAD] verify OK %1").arg(loc[i]);
            }
        }

        qInfo().noquote() << QString("[XFER][DOWNLOAD] batch OK files=%1 total=%2")
                             .arg(rem.size())
                             .arg(prettySize(totalBytes));

        return true;
    });
}


// -----------------------------------------------------------------------------
// downloadSelected()
// -----------------------------------------------------------------------------
// Explicit download action: user selects remote item(s) and chooses destination
// directory. Then we plan/expand selection and start downloads.
// -----------------------------------------------------------------------------
void FilesTab::downloadSelected()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, tr("Download"), tr("Not connected."));
        return;
    }

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Download"), tr("Select file(s) or folder(s) first."));
        return;
    }

    const QString destDir = QFileDialog::getExistingDirectory(
        this,
        tr("Select local destination folder"),
        m_localCwd.isEmpty() ? QDir::homePath() : m_localCwd);

    if (destDir.isEmpty()) return;

    QStringList remotePaths;
    for (const auto& idx : rows) {
        auto *it = m_remoteTable->item(idx.row(), 0);
        if (it) remotePaths << it->data(Qt::UserRole).toString();
    }

    startDownloadPaths(remotePaths, destDir);
}

// -----------------------------------------------------------------------------
// showLocalContextMenu()
// -----------------------------------------------------------------------------
// Local file browser context menu for basic actions.
// Operations are performed on the local filesystem directly.
// -----------------------------------------------------------------------------
void FilesTab::showLocalContextMenu(const QPoint& pos)
{
    if (!m_localView || !m_localModel) return;

    QMenu menu(this);

    QAction* actRename    = menu.addAction(tr("Rename…"));
    QAction* actNewFolder = menu.addAction(tr("New folder…"));
    menu.addSeparator();
    QAction* actCopyPath  = menu.addAction(tr("Copy path"));
    menu.addSeparator();
    QAction* actDelete    = menu.addAction(tr("Delete"));

    const QModelIndexList rows = m_localView->selectionModel()
                                    ? m_localView->selectionModel()->selectedRows()
                                    : QModelIndexList();

    // Enable/disable based on selection
    actRename->setEnabled(rows.size() == 1);
    actCopyPath->setEnabled(rows.size() >= 1);
    actDelete->setEnabled(rows.size() >= 1);

    const QAction* chosen = menu.exec(m_localView->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actRename)           renameLocalSelection();
    else if (chosen == actNewFolder)   newLocalFolder();
    else if (chosen == actCopyPath)    copyLocalPath();
    else if (chosen == actDelete)      deleteLocalSelection();
}


// -----------------------------------------------------------------------------
// showRemoteContextMenu()
// -----------------------------------------------------------------------------
// Remote file browser context menu.
// Remote operations are executed via SshClient:
// - rename/delete/new folder uses remote shell commands (mv/rm/mkdir)
// -----------------------------------------------------------------------------
void FilesTab::showRemoteContextMenu(const QPoint& pos)
{
    if (!m_remoteTable) return;

    QMenu menu(this);

    QAction* actRename    = menu.addAction(tr("Rename…"));
    QAction* actNewFolder = menu.addAction(tr("New folder…"));
    menu.addSeparator();
    QAction* actCopyPath  = menu.addAction(tr("Copy path"));
    menu.addSeparator();
    QAction* actDelete    = menu.addAction(tr("Delete"));

    const auto rows = m_remoteTable->selectionModel()
                        ? m_remoteTable->selectionModel()->selectedRows()
                        : QModelIndexList();

    actRename->setEnabled(rows.size() == 1);
    actCopyPath->setEnabled(rows.size() >= 1);
    actDelete->setEnabled(rows.size() >= 1);

    const QAction* chosen = menu.exec(m_remoteTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actRename)           renameRemoteSelection();
    else if (chosen == actNewFolder)   newRemoteFolder();
    else if (chosen == actCopyPath)    copyRemotePath();
    else if (chosen == actDelete)      deleteRemoteSelection();
}


// -----------------------------------------------------------------------------
// deleteLocalSelection()
// -----------------------------------------------------------------------------
// Deletes selected local files/folders recursively (with confirmation).
// WARNING: irreversible.
// -----------------------------------------------------------------------------
void FilesTab::deleteLocalSelection() {
    if (!m_localView || !m_localModel) return;

    const QModelIndexList rows = m_localView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Delete"), tr("Select one or more local items first."));
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

    const QString msg =
        tr("Delete %1 local item(s)?\n\nThis cannot be undone.")
            .arg(paths.size());

    if (QMessageBox::question(
            this,
            tr("Delete"),
            msg,
            QMessageBox::Yes | QMessageBox::No
        ) != QMessageBox::Yes) {
        return;
    }

    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
            QDir dir(p);
            if (!dir.removeRecursively()) {
                QMessageBox::warning(
                    this,
                    tr("Delete"),
                    tr("Failed to delete folder:\n%1").arg(p)
                );
                return;
            }
        } else {
            if (!QFile::remove(p)) {
                QMessageBox::warning(
                    this,
                    tr("Delete"),
                    tr("Failed to delete file:\n%1").arg(p)
                );
                return;
            }
        }

        // Refresh local view (keep cwd)
        m_localView->setRootIndex(m_localModel->index(m_localCwd));
    }
}

// -----------------------------------------------------------------------------
// deleteRemoteSelection()
// -----------------------------------------------------------------------------
// Deletes selected remote files/folders (with confirmation).
// Uses remote shell commands:
//   files:  rm -f
//   dirs :  rm -rf
// Paths are shell-quoted.
// -----------------------------------------------------------------------------
void FilesTab::deleteRemoteSelection()
{
    if (!m_ssh || !m_ssh->isConnected() || !m_remoteTable) {
        QMessageBox::information(this, tr("Delete"), tr("Not connected."));
        return;
    }

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Delete"), tr("Select one or more remote items first."));
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
        tr("Delete %1 remote item(s)?\n\nFolders will be deleted recursively.")
            .arg(items.size());

    if (QMessageBox::question(this, tr("Delete"), msg,
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    runTransfer(tr("Deleting %1 item(s)…").arg(items.size()),
                [this, items](QString* err) -> bool {

        for (const auto& x : items) {
            const QString cmd = x.isDir
                ? QString("rm -rf -- %1").arg(shQuote(x.path))
                : QString("rm -f  -- %1").arg(shQuote(x.path));

            QString out, e;
            if (!m_ssh->exec(cmd, &out, &e)) {
                if (err) {
                    *err = tr("Remote delete failed:\n%1\n%2")
                               .arg(x.path, e);
                }
                return false;
            }
        }
        return true;
    });

}

 // -----------------------------------------------------------------------------
// renameLocalSelection()
// -----------------------------------------------------------------------------
// Renames exactly one selected local item.
// Uses QDir::rename for directories and QFile::rename for files.
// -----------------------------------------------------------------------------
void FilesTab::renameLocalSelection()
{
    if (!m_localView || !m_localModel) return;

    const QModelIndexList rows = m_localView->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tr("Rename"), tr("Select exactly one local item."));
        return;
    }

    const QFileInfo fi = m_localModel->fileInfo(rows.first());
    if (!fi.exists()) return;

    bool ok = false;
    const QString oldName = fi.fileName();
    const QString newName = QInputDialog::getText(
        this, tr("Rename"), tr("New name:"), QLineEdit::Normal, oldName, &ok);

    if (!ok) return;
    const QString nn = newName.trimmed();
    if (nn.isEmpty() || nn == oldName) return;

    const QString oldPath = fi.absoluteFilePath();
    const QString newPath = QDir(fi.absolutePath()).filePath(nn);

    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, tr("Rename"), tr("Target name already exists."));
        return;
    }

    bool success = false;
    if (fi.isDir()) {
        QDir parent(fi.absolutePath());
        success = parent.rename(oldName, nn);
    } else {
        success = QFile::rename(oldPath, newPath);
    }

    if (!success) {
        QMessageBox::warning(this, tr("Rename"), tr("Rename failed."));
        return;
    }

    // Refresh local view (keep cwd)
    m_localView->setRootIndex(m_localModel->index(m_localCwd));
}

// -----------------------------------------------------------------------------
// renameRemoteSelection()
// -----------------------------------------------------------------------------
// Renames exactly one selected remote item using remote "mv".
// Paths are shell-quoted to avoid whitespace issues.
// -----------------------------------------------------------------------------
void FilesTab::renameRemoteSelection()
{
    if (!m_ssh || !m_ssh->isConnected() || !m_remoteTable) {
        QMessageBox::information(this, tr("Rename"), tr("Not connected."));
        return;
    }

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tr("Rename"), tr("Select exactly one remote item."));
        return;
    }

    auto* it = m_remoteTable->item(rows.first().row(), 0);
    if (!it) return;

    const QString oldPath = it->data(Qt::UserRole).toString();
    const QString oldName = QFileInfo(oldPath).fileName();

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, tr("Rename"), tr("New name:"), QLineEdit::Normal, oldName, &ok);

    if (!ok) return;
    const QString nn = newName.trimmed();
    if (nn.isEmpty() || nn == oldName) return;

    const QString parentDir = QFileInfo(oldPath).path();
    const QString newPath = joinRemote(parentDir, nn);

    runTransfer(tr("Renaming %1…").arg(oldName),
                [this, oldPath, newPath](QString* err) -> bool {
                    QString out, e;
                    const QString cmd = QString("mv -- %1 %2")
                                            .arg(shQuote(oldPath), shQuote(newPath));

                    if (!m_ssh->exec(cmd, &out, &e)) {
                        if (err) {
                            *err = tr("Rename failed:\n%1")
                                       .arg(e);
                        }
                        return false;
                    }
                    return true;
                });
}

// -----------------------------------------------------------------------------
// newLocalFolder()
// -----------------------------------------------------------------------------
// Creates a new folder in the current local working directory.
// -----------------------------------------------------------------------------
void FilesTab::newLocalFolder()
{
    if (!m_localModel || !m_localView) return;

    const QString base = m_localCwd.isEmpty()
        ? m_localModel->filePath(m_localView->rootIndex())
        : m_localCwd;

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New folder"), tr("Folder name:"), QLineEdit::Normal, tr("NewFolder"), &ok);

    if (!ok) return;
    const QString n = name.trimmed();
    if (n.isEmpty()) return;

    const QString path = QDir(base).filePath(n);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("New folder"), tr("That name already exists."));
        return;
    }

    if (!QDir(base).mkdir(n)) {
        QMessageBox::warning(this, tr("New folder"), tr("Failed to create folder."));
        return;
    }

    m_localView->setRootIndex(m_localModel->index(base));
}

// -----------------------------------------------------------------------------
// newRemoteFolder()
// -----------------------------------------------------------------------------
// Creates a new folder under current remote cwd using "mkdir -p".
// -----------------------------------------------------------------------------
void FilesTab::newRemoteFolder()
{
    if (!m_ssh || !m_ssh->isConnected()) {
        QMessageBox::information(this, tr("New folder"), tr("Not connected."));
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New folder"), tr("Folder name:"), QLineEdit::Normal, tr("NewFolder"), &ok);

    if (!ok) return;
    const QString n = name.trimmed();
    if (n.isEmpty()) return;

    const QString remotePath = joinRemote(m_remoteCwd, n);

    runTransfer(tr("Creating folder %1…").arg(n),
                [this, remotePath](QString* err) -> bool {
                    QString out, e;
                    const QString cmd = QString("mkdir -p -- %1").arg(shQuote(remotePath));
                    if (!m_ssh->exec(cmd, &out, &e)) {
                        if (err) {
                            *err = tr("Failed to create folder:\n%1")
                                       .arg(e);
                        }
                        return false;
                    }
                    return true;
                });
}

// -----------------------------------------------------------------------------
// copyLocalPath()
// -----------------------------------------------------------------------------
// Copies absolute local path of the first selected local item to clipboard.
// -----------------------------------------------------------------------------
void FilesTab::copyLocalPath()
{
    if (!m_localView || !m_localModel) return;

    const QModelIndexList rows = m_localView->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    const QFileInfo fi = m_localModel->fileInfo(rows.first());
    const QString path = fi.absoluteFilePath();
    if (path.isEmpty()) return;

    QGuiApplication::clipboard()->setText(path);
}

// -----------------------------------------------------------------------------
// copyRemotePath()
// -----------------------------------------------------------------------------
// Copies remote full path of the first selected remote item to clipboard.
// -----------------------------------------------------------------------------
void FilesTab::copyRemotePath()
{
    if (!m_remoteTable) return;

    const auto rows = m_remoteTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    auto* it = m_remoteTable->item(rows.first().row(), 0);
    if (!it) return;

    const QString path = it->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;

    QGuiApplication::clipboard()->setText(path);
}
