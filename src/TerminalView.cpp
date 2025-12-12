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



TerminalView::TerminalView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setAcceptDrops(true);
    setReadOnly(false);

    // Use a fixed-width font so it feels like a terminal
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(11);
    setFont(f);
}

TerminalView::~TerminalView() = default;

void TerminalView::setRemoteContext(const QString &user, const QString &host)
{
    m_remoteUser = user;
    m_remoteHost = host;
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    // Emit bytes so libssh shell can send them
    if (!event->text().isEmpty()) {
        const QByteArray data = event->text().toLocal8Bit();
        emit bytesTyped(data);
    }

    // Keep local echo / cursor etc.
    QPlainTextEdit::keyPressEvent(event);
}

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

    // Try to download the file first
    const QString localPath = downloadRemoteFileForDrag(word);
    if (localPath.isEmpty()) {
        qDebug() << "[TerminalView] SCP download failed for" << word;
        QPlainTextEdit::mouseMoveEvent(event);
        return;
    }

    // Start drag with local file URL
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

QString TerminalView::wordAtPosition(const QPoint &pos) const
{
    QTextCursor c = cursorForPosition(pos);
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}

// Helper: run `scp user@host:filename /tmp/pq-ssh-drops/filename`
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

    // 🔹 Wait a bit longer to avoid 0-byte portal/temporary file issues
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

