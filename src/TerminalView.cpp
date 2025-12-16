// src/TerminalView.cpp
#include "TerminalView.h"

#include <QKeyEvent>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QTimer>
#include <QFile>
#include <QPalette>

/*
 * TerminalView
 * ------------
 * A lightweight “terminal-like” widget built on QPlainTextEdit.
 *
 * Why this exists:
 *  - Provides a simple text surface with local echo
 *  - Emits typed bytes so a real SSH shell worker can send them
 *  - Implements drag & drop:
 *      * Drop local file into app -> emits fileDropped(path, bytes)
 *      * Drag selected word out of the terminal -> tries scp download
 *
 * Important architectural note:
 *  - This is NOT a real terminal emulator.
 *  - In the current codebase, the embedded real terminal is qtermwidget
 *    (CpunkTermWidget). TerminalView is used for simpler flows / legacy
 *    paths / prototyping. Keep its responsibilities modest.
 */

TerminalView::TerminalView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    // Enable accepting files dragged from OS into this widget
    setAcceptDrops(true);

    // This widget is “terminal-like”: it shows what you type (local echo)
    // and emits bytesTyped() so the SSH layer can forward input upstream.
    setReadOnly(false);

    // Remove the standard QTextEdit/QPlainTextEdit frame to blend with app theme
    setFrameShape(QFrame::NoFrame);
    setLineWidth(0);

    // Remove extra margins that can show parent background colors behind the viewport
    setContentsMargins(0, 0, 0, 0);
    setViewportMargins(0, 0, 0, 0);
    document()->setDocumentMargin(0);

    // Fixed-width font so it visually resembles a terminal
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(11);
    setFont(f);
}

TerminalView::~TerminalView() = default;

/*
 * setRemoteContext
 * ----------------
 * Provides the remote user/host needed for the “drag remote filename to desktop”
 * feature (implemented using scp in downloadRemoteFileForDrag()).
 *
 * Without this context, we do not attempt remote download on drag.
 */
void TerminalView::setRemoteContext(const QString &user, const QString &host)
{
    m_remoteUser = user;
    m_remoteHost = host;
}

/*
 * keyPressEvent
 * -------------
 * Emits the typed bytes (best-effort) for upstream SSH shell forwarding.
 *
 * Current behavior:
 *  - Uses event->text() (textual input) -> local8Bit conversion
 *  - Also calls base class so the widget echoes locally
 *
 * Notes:
 *  - This is not perfect for special keys (arrows, F-keys, etc.)
 *    because those are not plain text. A real terminal emulator
 *    (qtermwidget) handles that properly.
 */
void TerminalView::keyPressEvent(QKeyEvent *event)
{
    // Emit typed bytes so libssh shell can send them
    if (!event->text().isEmpty()) {
        const QByteArray data = event->text().toLocal8Bit();
        emit bytesTyped(data);
    }

    // Keep local echo / cursor movement etc.
    QPlainTextEdit::keyPressEvent(event);
}

/*
 * mousePressEvent / mouseMoveEvent
 * --------------------------------
 * Implements “drag file from remote” flow:
 *  - When user drags a word/selection out of terminal,
 *    interpret it as a remote filename.
 *  - Download it to a temp directory via scp.
 *  - Start a drag containing a local file URL.
 *
 * This is a UX shortcut for quick file retrieval.
 *
 * Security / UX notes:
 *  - scp may prompt (host key / password) in a detached context,
 *    depending on how ssh is configured.
 *  - Words containing spaces won’t work (WordUnderCursor logic).
 *  - This is “v1” behavior; future improvement: detect full paths.
 */
void TerminalView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
    }

    QPlainTextEdit::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // Only consider it a drag after crossing Qt’s drag threshold
    const int dist = (event->pos() - m_dragStartPos).manhattanLength();
    if (dist < QApplication::startDragDistance()) {
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // 1) Prefer explicit selection
    QString word;
    if (textCursor().hasSelection()) {
        word = textCursor().selectedText().trimmed();
    } else {
        // 2) Fallback: detect word at drag start
        word = wordAtPosition(m_dragStartPos).trimmed();
    }

    if (word.isEmpty()) {
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // We need remote context to do SCP
    if (m_remoteHost.isEmpty()) {
        qDebug() << "[TerminalView] No remote host set, cannot SCP drag for" << word;
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // Download the remote “word” as a filename using scp
    const QString localPath = downloadRemoteFileForDrag(word);
    if (localPath.isEmpty()) {
        qDebug() << "[TerminalView] SCP download failed for" << word;
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // Start drag with local file URL so desktop apps can receive it
    auto *mime = new QMimeData();
    mime->setText(localPath);

    QList<QUrl> urls;
    urls << QUrl::fromLocalFile(localPath);
    mime->setUrls(urls);

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);

    // Do not call base after starting a drag
}

/*
 * wordAtPosition
 * --------------
 * Returns the word under the mouse cursor position using Qt’s built-in
 * WordUnderCursor selection.
 */
QString TerminalView::wordAtPosition(const QPoint &pos) const
{
    QTextCursor c = cursorForPosition(pos);
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}

/*
 * downloadRemoteFileForDrag
 * -------------------------
 * Implements a simple scp-based fetch:
 *    scp user@host:<fileName> /tmp/pq-ssh-drops/<fileName>
 *
 * Behavior:
 *  - Creates a temp dir under QDir::tempPath()
 *  - Avoids overwriting existing files by adding _N suffix
 *  - Runs scp synchronously (blocking) up to 30 seconds
 *
 * NOTE:
 *  - This is intentionally “simple v1”.
 *  - Future: replace with libssh+sftp for consistency and better UX.
 */
QString TerminalView::downloadRemoteFileForDrag(const QString &fileName) const
{
    if (fileName.isEmpty())
        return QString();

    // Target directory for dragged files
    QString baseDir = QDir::tempPath() + "/pq-ssh-drops";
    QDir dir;
    if (!dir.exists(baseDir)) {
        if (!dir.mkpath(baseDir)) {
            qDebug() << "[TerminalView] Failed to create drop dir:" << baseDir;
            return QString();
        }
    }

    QString localPath = baseDir + "/" + fileName;

    // Avoid overwriting existing file blindly: if exists, append suffix
    if (QFileInfo::exists(localPath)) {
        int counter = 1;
        QString baseName = QFileInfo(fileName).completeBaseName();
        QString ext      = QFileInfo(fileName).suffix();
        while (QFileInfo::exists(localPath)) {
            QString numbered = ext.isEmpty()
                ? QString("%1_%2").arg(baseName).arg(counter)
                : QString("%1_%2.%3").arg(baseName).arg(counter).arg(ext);
            localPath = baseDir + "/" + numbered;
            ++counter;
        }
    }

    // Build remote spec: user@host:fileName or host:fileName
    QString remoteSpec;
    if (!m_remoteUser.isEmpty()) {
        remoteSpec = QStringLiteral("%1@%2:%3")
                        .arg(m_remoteUser, m_remoteHost, fileName);
    } else {
        remoteSpec = QStringLiteral("%1:%2")
                        .arg(m_remoteHost, fileName);
    }

    qDebug() << "[TerminalView] SCP drag:" << remoteSpec << "->" << localPath;

    QProcess scp;
    QStringList args;
    args << remoteSpec << localPath;

    scp.start(QStringLiteral("scp"), args);
    if (!scp.waitForStarted(3000)) {
        qDebug() << "[TerminalView] Failed to start scp";
        return QString();
    }

    // Wait up to 30 seconds – simple v1, blocking
    if (!scp.waitForFinished(30000)) {
        qDebug() << "[TerminalView] scp timed out";
        scp.kill();
        scp.waitForFinished();
        return QString();
    }

    if (scp.exitStatus() != QProcess::NormalExit || scp.exitCode() != 0) {
        qDebug() << "[TerminalView] scp failed. exitCode=" << scp.exitCode();
        qDebug() << "[TerminalView] stderr:" << scp.readAllStandardError();
        return QString();
    }

    if (!QFileInfo::exists(localPath)) {
        qDebug() << "[TerminalView] scp reported success but file missing:" << localPath;
        return QString();
    }

    return localPath;
}

/*
 * applyTerminalBackground
 * -----------------------
 * Forces a specific background color for both:
 *  - the widget frame area (Window)
 *  - the editable text area (Base)
 *  - the viewport palette + stylesheet as extra protection
 *
 * This exists because Qt style sheets and parent palettes can override
 * QPlainTextEdit backgrounds and cause “gray until first repaint” effects.
 */
void TerminalView::applyTerminalBackground(const QColor& bg)
{
    setAutoFillBackground(true);

    QPalette p = palette();
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::Base, bg);   // text area background
    setPalette(p);

    // Viewport is where QPlainTextEdit actually paints text/background
    viewport()->setAutoFillBackground(true);
    viewport()->setPalette(p);

    // Extra insurance against style fallbacks
    setStyleSheet(QString(
        "QPlainTextEdit { background:%1; border:0px; padding:0px; }"
        "QPlainTextEdit::viewport { background:%1; }"
    ).arg(bg.name()));
}

/*
 * dragEnterEvent / dropEvent
 * --------------------------
 * Implements local file drop INTO the terminal area:
 *  - Accept file URLs
 *  - Read file bytes (after a short delay)
 *  - Emit fileDropped(path, content)
 *
 * Delay rationale:
 *  - Some desktop environments provide a temporary “portal” file that
 *    becomes readable slightly later, which can otherwise appear as 0 bytes.
 */
void TerminalView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void TerminalView::dropEvent(QDropEvent *event)
{
    const QMimeData *md = event->mimeData();
    if (!md || !md->hasUrls()) {
        event->ignore();
        return;
    }

    const QList<QUrl> urls = md->urls();
    if (urls.isEmpty()) {
        event->ignore();
        return;
    }

    const QString filePath = urls.first().toLocalFile();
    if (filePath.isEmpty()) {
        qWarning() << "Drop event: first URL has no local file path";
        event->ignore();
        return;
    }

    QFileInfo fi(filePath);
    qDebug() << "[DROP] initial path:" << filePath
             << "exists:" << fi.exists()
             << "sizeOnDisk:" << fi.size();

    event->acceptProposedAction();

    // Wait a bit longer to avoid 0-byte portal/temporary file issues
    QTimer::singleShot(200, this, [this, filePath]() {
        QFileInfo fi2(filePath);
        qDebug() << "[DROP] after delay path:" << filePath
                 << "exists:" << fi2.exists()
                 << "sizeOnDisk:" << fi2.size();

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Failed to open dropped file:" << filePath
                       << "error:" << file.errorString();
            return;
        }

        const QByteArray content = file.readAll();
        file.close();

        qDebug() << "[DROP] read bytes:" << content.size();

        emit fileDropped(filePath, content);
    });
}
