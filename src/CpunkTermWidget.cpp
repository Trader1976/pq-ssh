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
#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFont>
#include <functional>

// -----------------------------
// Drag & Drop helpers
// -----------------------------
static QString firstLocalFilePath(const QMimeData *md)
{
    if (!md || !md->hasUrls()) return {};
    const auto urls = md->urls();
    if (urls.isEmpty()) return {};
    if (!urls.first().isLocalFile()) return {};
    return urls.first().toLocalFile();
}

static void tryReadWithRetries(
    QObject *ctx,
    const QString &filePath,
    int attempt,
    int maxAttempts,
    const std::function<void(const QByteArray&)> &onOk,
    const std::function<void(const QString&)> &onFail)
{
    QFile f(filePath);
    if (f.open(QIODevice::ReadOnly)) {
        onOk(f.readAll());
        return;
    }
    if (attempt >= maxAttempts) {
        onFail(f.errorString());
        return;
    }
    static const int delays[] = {120, 300, 700, 1200};
    QTimer::singleShot(delays[qMin(attempt, 3)], ctx, [=]() {
        tryReadWithRetries(ctx, filePath, attempt + 1, maxAttempts, onOk, onFail);
    });
}

void CpunkTermWidget::setupDropInterceptor()
{
    setAcceptDrops(true);
    installEventFilter(this);

    const auto kids = findChildren<QWidget*>();
    for (QWidget *w : kids) {
        w->setAcceptDrops(true);
        w->installEventFilter(this);
    }
}

// -----------------------------
// Constructor
// -----------------------------
CpunkTermWidget::CpunkTermWidget(int historyLines, QWidget *parent)
    : QTermWidget(0, parent) // ✅ prevent auto-start local shell
{
    setHistorySize(historyLines);      // ✅ apply history lines properly

    // IMPORTANT: prevent app QSS from affecting terminal rendering
    setStyleSheet(QString());
    for (QWidget *w : findChildren<QWidget*>())
        w->setStyleSheet(QString());

    // Build a NORMAL monospace font
    QFont f(QStringLiteral("DejaVu Sans Mono"));
    if (!QFontInfo(f).exactMatch()) {
        f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    f.setStyleHint(QFont::TypeWriter);
    f.setFixedPitch(true);
    f.setKerning(false);
    f.setBold(false);
    f.setWeight(QFont::Normal);
    f.setPointSize(11);

    auto applyTerminalLook = [this, f]() {
        setTerminalOpacity(1.0);
        setTerminalFont(f);

        setStyleSheet(QString());
        for (QWidget *w : findChildren<QWidget*>()) {
            w->setStyleSheet(QString());
            QFont wf = w->font();
            wf.setBold(false);
            wf.setWeight(QFont::Normal);
            w->setFont(wf);
        }
    };

    applyTerminalLook();
    QTimer::singleShot(0, this, applyTerminalLook);

    // Copy / Paste actions
    setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *copyAct = new QAction(tr("Copy"), this);
    copyAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    addAction(copyAct);

    auto *pasteAct = new QAction(tr("Paste"), this);
    pasteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    addAction(pasteAct);

    auto *pasteAct2 = new QAction(tr("Paste (Shift+Insert)"), this);
    pasteAct2->setShortcut(QKeySequence(QStringLiteral("Shift+Insert")));
    addAction(pasteAct2);

    connect(copyAct, &QAction::triggered, this, [this]() {
        const QString sel = selectedText();
        if (!sel.isEmpty())
            QGuiApplication::clipboard()->setText(sel);
    });

    auto doPaste = [this]() {
        const QString text = QGuiApplication::clipboard()->text();
        if (!text.isEmpty())
            sendText(text);
    };

    connect(pasteAct,  &QAction::triggered, this, doPaste);
    connect(pasteAct2, &QAction::triggered, this, doPaste);

    setupDropInterceptor();
}


// -----------------------------
// Event filter (drop)
// -----------------------------
bool CpunkTermWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::DragEnter) {
        auto *de = static_cast<QDragEnterEvent*>(event);
        if (!firstLocalFilePath(de->mimeData()).isEmpty()) {
            de->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent*>(event);
        const QString filePath = firstLocalFilePath(drop->mimeData());
        if (!filePath.isEmpty()) {
            drop->acceptProposedAction();

            tryReadWithRetries(
                this, filePath, 0, 4,
                [this, filePath](const QByteArray &content) {
                    emit fileDropped(filePath, content);
                },
                [filePath](const QString &err) {
                    qWarning() << "CpunkTermWidget: failed to read dropped file:"
                               << filePath << err;
                }
            );

            return true;
        }
    }

    return QTermWidget::eventFilter(obj, event);
}
