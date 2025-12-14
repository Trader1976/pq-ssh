#include "Logger.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QMutex>
#include <QDebug>

static QFile* g_file = nullptr;
static QMutex g_mutex;
static QString g_path;

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

static void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    QMutexLocker lock(&g_mutex);
    if (!g_file || !g_file->isOpen())
        return;

    QTextStream out(g_file);
    out.setCodec("UTF-8");

    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const QString lvl = levelToString(type);
    const QString where = (ctx.file && ctx.function)
        ? QString("%1:%2 %3").arg(QFileInfo(ctx.file).fileName()).arg(ctx.line).arg(ctx.function)
        : QString();

    out << ts << " [" << lvl << "] ";
    if (!where.isEmpty()) out << where << " - ";
    out << msg << "\n";
    out.flush();

    if (type == QtFatalMsg) abort();
}

static void rotateIfNeeded(const QString& path, qint64 maxBytes = 2 * 1024 * 1024, int keep = 3)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < maxBytes) return;

    for (int i = keep; i >= 1; --i) {
        const QString older = path + "." + QString::number(i);
        const QString newer = path + "." + QString::number(i + 1);
        if (QFileInfo::exists(newer)) QFile::remove(newer);
        if (QFileInfo::exists(older)) QFile::rename(older, newer);
    }
    QFile::rename(path, path + ".1");
}

namespace Logger {

void install(const QString& appName)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                        + "/logs";
    QDir().mkpath(dir);

    g_path = dir + "/" + appName + ".log";
    rotateIfNeeded(g_path);

    g_file = new QFile(g_path);
    g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    qInstallMessageHandler(handler);

    qInfo() << "Logger initialized:" << g_path;
}

QString logFilePath()
{
    return g_path;
}

} // namespace Logger
