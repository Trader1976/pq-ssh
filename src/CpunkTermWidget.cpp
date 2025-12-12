#include "CpunkTermWidget.h"

#include <QEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QDebug>
#include <QWidget>

CpunkTermWidget::CpunkTermWidget(int historyLines, QWidget *parent)
    : QTermWidget(historyLines, parent)
{
    setupDropInterceptor();
}

void CpunkTermWidget::setupDropInterceptor()
{
    // Accept drops on the QTermWidget itself
    setAcceptDrops(true);
    installEventFilter(this);

    // Also install on all child widgets (TerminalDisplay is one of them)
    const auto children = findChildren<QWidget *>();
    for (QWidget *w : children) {
        w->setAcceptDrops(true);
        w->installEventFilter(this);
        // qDebug() << "CpunkTermWidget: installed event filter on child"
        //          << w->metaObject()->className();
    }
}

static QString firstLocalFilePath(const QMimeData *md)
{
    if (!md || !md->hasUrls())
        return {};

    const QList<QUrl> urls = md->urls();
    if (urls.isEmpty())
        return {};

    const QUrl &u = urls.first();
    if (!u.isLocalFile())
        return {};

    return u.toLocalFile();
}

static void tryReadWithRetries(QObject *ctx,
                               const QString &filePath,
                               int attempt,
                               int maxAttempts,
                               std::function<void(const QByteArray&)> onOk,
                               std::function<void(const QString&)> onFail)
{
    QFile f(filePath);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray content = f.readAll();
        f.close();
        onOk(content);
        return;
    }

    // Still not readable → retry (portal delay / slow mount)
    if (attempt >= maxAttempts) {
        onFail(f.errorString());
        return;
    }

    // Backoff timings: 120ms, 300ms, 700ms...
    const int delays[] = { 120, 300, 700, 1200 };
    const int delay = delays[qMin(attempt, (int)(sizeof(delays)/sizeof(delays[0])) - 1)];

    QTimer::singleShot(delay, ctx, [=]() {
        tryReadWithRetries(ctx, filePath, attempt + 1, maxAttempts, onOk, onFail);
    });
}

bool CpunkTermWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::DragEnter) {
        auto *de = static_cast<QDragEnterEvent*>(event);
        const QString filePath = firstLocalFilePath(de->mimeData());
        if (!filePath.isEmpty()) {
            de->acceptProposedAction();
            return true; // stop default handling
        }
        // Let it fall through if not a local file
    }
    else if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent*>(event);
        const QString filePath = firstLocalFilePath(drop->mimeData());
        if (!filePath.isEmpty()) {
            drop->acceptProposedAction();

            // Read file with retries (handles xdg-desktop-portal delay)
            tryReadWithRetries(
                this,
                filePath,
                0,   // attempt
                4,   // max attempts
                [this, filePath](const QByteArray &content) {
                    QFileInfo fi(filePath);
                    qDebug() << "[TERM DROP] path:" << filePath
                             << "exists:" << fi.exists()
                             << "sizeOnDisk:" << fi.size()
                             << "read bytes:" << content.size();

                    emit fileDropped(filePath, content);
                },
                [filePath](const QString &err) {
                    qWarning() << "CpunkTermWidget: failed to open dropped file:"
                               << filePath << err;
                }
            );

            return true; // handled
        }
        // else: not local file, let it pass through
    }

    // Let QObject handle default filtering for everything else
    return QObject::eventFilter(obj, event);
}
