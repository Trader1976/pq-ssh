#include "Logger.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QMutex>
#include <QDebug>

// =====================================================
// Global logger state (process-wide)
// =====================================================
//
// We deliberately use a single global log file and mutex.
// Qt's message handler is global by design, so this is
// the correct scope for these objects.
//

static QFile*  g_file  = nullptr;  // Open log file handle
static QMutex  g_mutex;             // Guards concurrent writes
static QString g_path;              // Absolute path to log file

// =====================================================
// Log level mapping
// =====================================================
//
// Converts Qt message types into stable, human-readable
// strings suitable for long-term logs.
//

static QString levelToString(QtMsgType t) {
    switch (t) {
        case QtDebugMsg:    return "DEBUG";
#if (QT_VERSION >= QT_VERSION_CHECK(5, 5, 0))
        case QtInfoMsg:     return "INFO";
#endif
        case QtWarningMsg:  return "WARN";
        case QtCriticalMsg: return "ERROR";
        case QtFatalMsg:    return "FATAL";
    }
    return "LOG";
}

// =====================================================
// Qt message handler
// =====================================================
//
// This function is installed via qInstallMessageHandler()
// and receives *all* Qt logging output (qDebug/qInfo/etc).
//
// Design goals:
// - Thread-safe
// - Append-only
// - Human-readable
// - Safe to call from any thread
//

static void handler(QtMsgType type,
                    const QMessageLogContext& ctx,
                    const QString& msg)
{
    QMutexLocker lock(&g_mutex);

    // If logging is not initialized yet, silently drop messages.
    // (Early startup logging is intentionally minimal.)
    if (!g_file || !g_file->isOpen())
        return;

    QTextStream out(g_file);
    out.setCodec("UTF-8");

    // Timestamp with millisecond precision for debugging races
    const QString ts =
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");

    const QString lvl = levelToString(type);

    // Optional source context (only if Qt provides it)
    const QString where =
        (ctx.file && ctx.function)
            ? QString("%1:%2 %3")
                  .arg(QFileInfo(ctx.file).fileName())
                  .arg(ctx.line)
                  .arg(ctx.function)
            : QString();

    // Log line format:
    // 2025-01-01 12:34:56.789 [INFO] MainWindow.cpp:123 func() - message
    out << ts << " [" << lvl << "] ";
    if (!where.isEmpty())
        out << where << " - ";
    out << msg << "\n";
    out.flush();

    // Fatal errors must terminate the process
    if (type == QtFatalMsg)
        abort();
}

// =====================================================
// Log rotation
// =====================================================
//
// Simple size-based rotation:
//
//   app.log        (current)
//   app.log.1      (most recent old)
//   app.log.2
//   app.log.3
//
// Rotation happens *once* at startup.
//

static void rotateIfNeeded(const QString& path,
                           qint64 maxBytes = 2 * 1024 * 1024,
                           int keep = 3)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < maxBytes)
        return;

    // Rotate from highest index down to avoid overwriting
    for (int i = keep; i >= 1; --i) {
        const QString older = path + "." + QString::number(i);
        const QString newer = path + "." + QString::number(i + 1);
        if (QFileInfo::exists(newer))
            QFile::remove(newer);
        if (QFileInfo::exists(older))
            QFile::rename(older, newer);
    }

    QFile::rename(path, path + ".1");
}

// =====================================================
// Public Logger API
// =====================================================

namespace Logger {

// -----------------------------------------------------
// install()
// -----------------------------------------------------
//
// Initializes logging for the application.
// Must be called ONCE during application startup.
//
// Responsibilities:
// - Resolve platform-appropriate log directory
// - Rotate existing logs
// - Open log file
// - Install Qt global message handler
//

void install(const QString& appName)
{
    // Platform-correct writable location:
    //  Linux: ~/.local/share/<app>/logs
    //  Windows: AppData/Local/<app>/logs
    //  macOS: ~/Library/Application Support/<app>/logs
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + "/logs";

    QDir().mkpath(dir);

    g_path = dir + "/" + appName + ".log";

    // Rotate old logs before opening
    rotateIfNeeded(g_path);

    g_file = new QFile(g_path);
    g_file->open(QIODevice::WriteOnly |
                 QIODevice::Append |
                 QIODevice::Text);

    // Install global handler
    qInstallMessageHandler(handler);

    // First line in the log
    qInfo() << "Logger initialized:" << g_path;
}

// -----------------------------------------------------
// logFilePath()
// -----------------------------------------------------
//
// Returns the absolute path of the active log file.
// Used by UI actions (Help → Open log file).
//

QString logFilePath()
{
    return g_path;
}

} // namespace Logger
