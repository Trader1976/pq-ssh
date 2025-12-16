// src/CpunkTermWidget.cpp
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

/*
    CpunkTermWidget
    --------------
    Thin wrapper around QTermWidget (qtermwidget) used by pq-ssh.

    Goals:
    - Never auto-start a local shell (security + UX).
    - Keep terminal appearance stable (avoid global app QSS/QWidget styles leaking in).
    - Provide copy/paste shortcuts that match common terminal behavior.
    - Support drag & drop file upload (we emit fileDropped(path, bytes) so caller can decide what to do).

    Important note:
    QTermWidget is a QWidget tree internally. Some drag/drop events arrive to child widgets,
    so we install an event filter on this widget AND all child widgets to catch drops reliably.
*/

// ============================================================================
// Drag & Drop helpers
// ============================================================================

/// Extract the first local file path from a mime payload.
/// Returns empty string if there is no local file.
static QString firstLocalFilePath(const QMimeData *md)
{
    if (!md || !md->hasUrls()) return {};
    const auto urls = md->urls();
    if (urls.isEmpty()) return {};
    if (!urls.first().isLocalFile()) return {};
    return urls.first().toLocalFile();
}

/*
    Some platforms (Windows especially) and some apps provide a file URL immediately,
    but the file itself may not be readable yet (0-byte staging files, delayed writes, portals).

    tryReadWithRetries() tries to open+read the file a few times with increasing delays.
    - ctx: QObject context for QTimer lifetime
    - attempt/maxAttempts: retry counters
    - onOk: called with file bytes
    - onFail: called with final error string
*/
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

    // If we've exhausted retries, report the last QFile error.
    if (attempt >= maxAttempts) {
        onFail(f.errorString());
        return;
    }

    // Simple backoff schedule (ms). Kept short so UX feels instant.
    static const int delays[] = {120, 300, 700, 1200};

    QTimer::singleShot(delays[qMin(attempt, 3)], ctx, [=]() {
        tryReadWithRetries(ctx, filePath, attempt + 1, maxAttempts, onOk, onFail);
    });
}

/*
    Install drop handling on the terminal widget and its children.

    Why children too?
    QTermWidget is composed of internal widgets; depending on where the mouse is,
    the drop event can be delivered to a child rather than the top-level QTermWidget.

    By filtering events on the whole subtree we reliably catch:
    - DragEnter (to accept)
    - Drop (to read file bytes and emit fileDropped)
*/
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

// ============================================================================
// Constructor
// ============================================================================

CpunkTermWidget::CpunkTermWidget(int historyLines, QWidget *parent)
    // QTermWidget(0, parent):
    // The "0" here is important. Some QTermWidget constructors can auto-start a local shell.
    // We explicitly prevent that, because pq-ssh should never accidentally provide a local shell.
    : QTermWidget(0, parent)
{
    // History buffer: how many lines qtermwidget keeps (scrollback)
    setHistorySize(historyLines);

    /*
        IMPORTANT: protect terminal rendering from global app stylesheet (QSS).

        pq-ssh uses a global dark theme via AppTheme. If we let QSS cascade into the terminal,
        it can cause:
        - wrong background (gray) until first repaint
        - mixed colors (parent background bleeding through)
        - fonts becoming bold or incorrect weights

        Clearing style sheets here keeps qtermwidget in control of its own painting.
    */
    setStyleSheet(QString());
    for (QWidget *w : findChildren<QWidget*>())
        w->setStyleSheet(QString());

    // Build a "normal" monospace font (no bold, fixed pitch, stable metrics)
    QFont f(QStringLiteral("DejaVu Sans Mono"));
    if (!QFontInfo(f).exactMatch()) {
        // Fallback to system fixed font if preferred font not available
        f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    f.setStyleHint(QFont::TypeWriter);
    f.setFixedPitch(true);
    f.setKerning(false);
    f.setBold(false);
    f.setWeight(QFont::Normal);
    f.setPointSize(11);

    /*
        Apply terminal look.

        We apply immediately, and again on the next event loop turn (singleShot 0),
        because some internal child widgets may not exist until after construction.
        Re-applying ensures the font/opacity wins over any later initialization.
    */
    auto applyTerminalLook = [this, f]() {
        setTerminalOpacity(1.0);
        setTerminalFont(f);

        // Re-clear any stylesheets that might have been set by parent or qtermwidget init.
        setStyleSheet(QString());
        for (QWidget *w : findChildren<QWidget*>()) {
            w->setStyleSheet(QString());

            // Ensure no unexpected bold fonts sneak in from style propagation.
            QFont wf = w->font();
            wf.setBold(false);
            wf.setWeight(QFont::Normal);
            w->setFont(wf);
        }
    };

    applyTerminalLook();
    QTimer::singleShot(0, this, applyTerminalLook);

    // ------------------------------------------------------------------------
    // Copy / Paste actions
    // ------------------------------------------------------------------------
    /*
        We expose actions rather than manually handling key events because:
        - qtermwidget already handles a lot of input logic
        - QAction shortcuts integrate nicely with Qt focus/menus

        Terminal-friendly defaults:
        - Ctrl+Shift+C : Copy selection
        - Ctrl+Shift+V : Paste clipboard text
        - Shift+Insert : Paste (classic terminal)
    */
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

    // Enable drag & drop file reading (emits fileDropped)
    setupDropInterceptor();
}

// ============================================================================
// Event filter (DragEnter / Drop)
// ============================================================================

/*
    We filter DragEnter and Drop events for the widget tree.

    Behavior:
    - DragEnter: accept if payload contains a local file path
    - Drop: read file bytes (with retries) and emit fileDropped(path, bytes)

    Note:
    This is intentionally "transport only". It doesn't decide how to upload.
    MainWindow/SshClient decide what to do with fileDropped().
*/
bool CpunkTermWidget::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj);

    if (event->type() == QEvent::DragEnter) {
        auto *de = static_cast<QDragEnterEvent*>(event);
        if (!firstLocalFilePath(de->mimeData()).isEmpty()) {
            de->acceptProposedAction();
            return true; // stop propagation (terminal won't reject it)
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

    // Fall back to QTermWidget processing for everything else
    return QTermWidget::eventFilter(obj, event);
}
