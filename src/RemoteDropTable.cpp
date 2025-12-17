#include "RemoteDropTable.h"
#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>

static bool hasFileUrls(const QMimeData *m)
{
    if (!m || !m->hasUrls()) return false;
    for (const QUrl& u : m->urls())
        if (u.isLocalFile()) return true;
    return false;
}

void RemoteDropTable::dragEnterEvent(QDragEnterEvent *e)
{
    if (hasFileUrls(e->mimeData())) e->acceptProposedAction();
    else e->ignore();
}

void RemoteDropTable::dragMoveEvent(QDragMoveEvent *e)
{
    if (hasFileUrls(e->mimeData())) e->acceptProposedAction();
    else e->ignore();
}

void RemoteDropTable::dropEvent(QDropEvent *e)
{
    if (!hasFileUrls(e->mimeData())) { e->ignore(); return; }

    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) {
        if (u.isLocalFile()) paths << u.toLocalFile();
    }
    if (!paths.isEmpty())
        emit filesDropped(paths);

    e->acceptProposedAction();
}
