#include "RemoteDropTable.h"

#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QSet>

// RemoteDropTable
// --------------
// A QTableWidget that supports *both* directions of file transfer UX:
//
// 1) Local -> Remote (drop local files onto the table)
//    - We accept drops that contain local file URLs.
//    - On drop, we emit filesDropped(QStringList localPaths).
//
// 2) Remote -> Local (drag rows out of the table)
//    - We provide custom mimeData that contains the selected remote paths.
//    - The payload is a newline-separated list stored under a custom MIME type:
//        "application/x-pqssh-remote-paths"
//
// Note: This widget does not perform any transfer itself. It only packages
// user intent into signals / mime payloads for higher-level code to act on.

RemoteDropTable::RemoteDropTable(QWidget *parent)
    : QTableWidget(parent)
{
    // Enable BOTH:
    // - dragging rows out (remote -> local)
    // - dropping local files in (local -> remote)
    setAcceptDrops(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    // Transfers are row-based: selection should operate on whole rows.
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
}

// Returns true if the mime payload contains at least one *local* file URL.
// We only accept drops that originate from the local filesystem (not remote URLs).
static bool hasLocalUrls(const QMimeData* md)
{
    if (!md || !md->hasUrls()) return false;
    for (const QUrl& u : md->urls()) {
        if (u.isLocalFile()) return true;
    }
    return false;
}

// Accept drag entering the table if the payload includes local file URLs.
// Otherwise, fall back to the default QTableWidget behavior.
void RemoteDropTable::dragEnterEvent(QDragEnterEvent *e)
{
    if (hasLocalUrls(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QTableWidget::dragEnterEvent(e);
}

// While dragging over the table, keep accepting the action for local file URLs
// so the cursor feedback remains correct (Copy).
void RemoteDropTable::dragMoveEvent(QDragMoveEvent *e)
{
    if (hasLocalUrls(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QTableWidget::dragMoveEvent(e);
}

// Handle drop (Local -> Remote):
// - Extract all local file paths from the dropped URLs.
// - Emit filesDropped(paths) so the Files tab / SFTP layer can upload them.
void RemoteDropTable::dropEvent(QDropEvent *e)
{
    if (!hasLocalUrls(e->mimeData())) {
        QTableWidget::dropEvent(e);
        return;
    }

    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) {
        if (u.isLocalFile())
            paths << u.toLocalFile();
    }

    if (!paths.isEmpty()) {
        emit filesDropped(paths);
        e->acceptProposedAction();
        return;
    }

    // If we got here, URLs existed but none were local files.
    QTableWidget::dropEvent(e);
}

// Package selected rows as a custom MIME payload (Remote -> Local).
// The receiver (e.g., a drop target in the OS file manager integration
// or your app's local download handler) can parse newline-separated paths.
QMimeData* RemoteDropTable::mimeData(const QList<QTableWidgetItem*> items) const
{
    if (items.isEmpty())
        return nullptr;

    // Unique rows: the selection may include multiple cells per row.
    QSet<int> rows;
    for (auto *it : items) {
        if (it) rows.insert(it->row());
    }

    QStringList remotePaths;
    remotePaths.reserve(rows.size());

    // Convention: column 0 stores fullPath in Qt::UserRole (per your table fill code).
    for (int r : rows) {
        auto *nameItem = item(r, 0);
        if (!nameItem) continue;

        const QString path = nameItem->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            remotePaths << path;
    }

    if (remotePaths.isEmpty())
        return nullptr;

    auto *mime = new QMimeData();
    mime->setData("application/x-pqssh-remote-paths",
                  remotePaths.join('\n').toUtf8());
    return mime;
}
