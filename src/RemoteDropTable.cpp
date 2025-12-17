#include "RemoteDropTable.h"

#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QSet>

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

    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
}

static bool hasLocalUrls(const QMimeData* md)
{
    if (!md || !md->hasUrls()) return false;
    for (const QUrl& u : md->urls()) {
        if (u.isLocalFile()) return true;
    }
    return false;
}

void RemoteDropTable::dragEnterEvent(QDragEnterEvent *e)
{
    if (hasLocalUrls(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QTableWidget::dragEnterEvent(e);
}

void RemoteDropTable::dragMoveEvent(QDragMoveEvent *e)
{
    if (hasLocalUrls(e->mimeData())) {
        e->acceptProposedAction();
        return;
    }
    QTableWidget::dragMoveEvent(e);
}

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

    QTableWidget::dropEvent(e);
}

QMimeData* RemoteDropTable::mimeData(const QList<QTableWidgetItem*> items) const
{
    if (items.isEmpty())
        return nullptr;

    // Unique rows
    QSet<int> rows;
    for (auto *it : items) {
        if (it) rows.insert(it->row());
    }

    QStringList remotePaths;
    remotePaths.reserve(rows.size());

    // Column 0 stores fullPath in Qt::UserRole (per your table fill code)
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
