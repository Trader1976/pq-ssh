// Logger.cpp
#include "Logger.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <QAtomicInt>
#include <QDebug>

#include <cstdio>     // fprintf
#include <cstdlib>    // abort

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

// =====================================================
// Global logger state (process-wide)
// =====================================================

static QFile*     g_file  = nullptr;   // Open log file handle
static QMutex     g_mutex;             // Guards concurrent writes
static QString    g_path;              // Absolute path to log file
static QAtomicInt g_level(1);          // 0=Errors only, 1=Normal, 2=Debug
static QString    g_pathOverride;

// Prevent recursion if something inside handler triggers Qt logging again
static thread_local bool g_inHandler = false;

// =====================================================
// Log level mapping
// =====================================================

static QString levelToString(QtMsgType t)
{
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
// Filtering by runtime logging level
// =====================================================
//
// 0 = Errors only: WARN/ERROR/FATAL
// 1 = Normal:      INFO/WARN/ERROR/FATAL
// 2 = Debug:       DEBUG/INFO/WARN/ERROR/FATAL
//
static bool allowMessage(QtMsgType type)
{
    const int lvl = g_level.loadAcquire();

    if (lvl <= 0) {
        return (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
    }

    if (lvl == 1) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 5, 0))
        return (type == QtInfoMsg || type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
#else
        // Qt < 5.5 has no QtInfoMsg
        return (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
#endif
    }

    // lvl >= 2
    return true;
}

// =====================================================
// Message normalization (one record = one physical line)
// =====================================================
static QString normalizeMessage(QString s)
{
    // Normalize CRLF/CR to LF
    s.replace("\r\n", "\n");
    s.replace('\r', '\n');

    // Enforce one-line-per-record
    s.replace('\n', ' ');
    s.replace('\t', ' ');

    // Collapse repeated whitespace
    s = s.simplified();

    return s;
}

// =====================================================
// Qt message handler
// =====================================================
static void handler(QtMsgType type,
                    const QMessageLogContext& ctx,
                    const QString& msg)
{
    if (!allowMessage(type)) {
        if (type == QtFatalMsg) abort(); // never suppress fatal
        return;
    }

    if (g_inHandler) {
        if (type == QtFatalMsg) abort();
        return;
    }
    g_inHandler = true;

    {
        QMutexLocker lock(&g_mutex);

        const QString ts  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        const QString lvl = levelToString(type);

        const QString where =
            (ctx.file && ctx.function)
                ? QString("%1:%2 %3")
                      .arg(QFileInfo(QString::fromUtf8(ctx.file)).fileName())
                      .arg(ctx.line)
                      .arg(ctx.function)
                : QString();

        const QString cleanMsg = normalizeMessage(msg);

        // If logging isn't initialized or file isn't open, fallback to stderr.
        if (!g_file || !g_file->isOpen()) {
            QByteArray utf8;
            if (where.isEmpty())
                utf8 = QString("%1 [%2] %3").arg(ts, lvl, cleanMsg).toUtf8();
            else
                utf8 = QString("%1 [%2] %3 - %4").arg(ts, lvl, where, cleanMsg).toUtf8();

            std::fprintf(stderr, "%s\n", utf8.constData());
            std::fflush(stderr);

            if (type == QtFatalMsg) abort();
            g_inHandler = false;
            return;
        }

        QTextStream out(g_file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        out.setEncoding(QStringConverter::Utf8);
#else
        out.setCodec("UTF-8");
#endif

        out << ts << " [" << lvl << "] ";
        if (!where.isEmpty())
            out << where << " - ";
        out << cleanMsg << "\n";
        out.flush();

        if (type == QtFatalMsg)
            abort();
    }

    g_inHandler = false;
}



// =====================================================
// Log rotation (size-based)
// =====================================================

static void rotateIfNeeded(const QString& path,
                           qint64 maxBytes = 2 * 1024 * 1024,
                           int keep = 3)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < maxBytes)
        return;

    // Rotate: .3 -> .4, .2 -> .3, .1 -> .2, log -> .1
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

void install(const QString& appName)
{
    const QString defaultDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir().mkpath(defaultDir);

    const QString defaultPath = defaultDir + "/" + appName + ".log";

    const QString chosenPath =
        (!g_pathOverride.trimmed().isEmpty())
            ? QDir::cleanPath(g_pathOverride.trimmed())
            : defaultPath;

    g_path = chosenPath;

    QDir().mkpath(QFileInfo(g_path).absolutePath());
    rotateIfNeeded(g_path);

    {
        QMutexLocker lock(&g_mutex);
        if (g_file) {
            if (g_file->isOpen()) g_file->close();
            delete g_file;
            g_file = nullptr;
        }

        g_file = new QFile(g_path);
        if (!g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            std::fprintf(stderr, "Logger: failed to open log file: %s\n",
                         g_path.toUtf8().constData());
            std::fflush(stderr);
        }
    }

    qInstallMessageHandler(handler);
    qInfo().noquote() << QString("Logger initialized: %1").arg(g_path);
}

QString logFilePath()
{
    return g_path;
}

void setLogFilePathOverride(const QString& absoluteFilePath)
{
    QMutexLocker lock(&g_mutex);

    g_pathOverride = QDir::cleanPath(absoluteFilePath.trimmed());

    // If cleared, just keep current log open; next install() will use default.
    if (g_pathOverride.isEmpty())
        return;

    QDir().mkpath(QFileInfo(g_pathOverride).absolutePath());
    rotateIfNeeded(g_pathOverride);

    if (g_file) {
        if (g_file->isOpen()) g_file->close();
        delete g_file;
        g_file = nullptr;
    }

    g_file = new QFile(g_pathOverride);
    if (g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        g_path = g_pathOverride;
    } else {
        std::fprintf(stderr, "Logger: failed to open override log file: %s\n",
                     g_pathOverride.toUtf8().constData());
        std::fflush(stderr);
    }
}

// 0=Errors only, 1=Normal, 2=Debug
void setLogLevel(int level)
{
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    g_level.storeRelease(level);
}

int logLevel()
{
    return g_level.loadAcquire();
}

} // namespace Logger

QString Logger::logDirPath()
{
    const QString p = logFilePath().trimmed();
    if (p.isEmpty()) return QString();
    return QFileInfo(p).absolutePath();
}
