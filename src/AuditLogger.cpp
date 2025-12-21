// AuditLogger.cpp
#include "AuditLogger.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QSysInfo>
#include <QThread>
#include <QCryptographicHash>

// =====================================================
// Global audit logger state (process-wide)
// =====================================================

static QMutex   g_auditMutex;
static QString  g_appName;
static QString  g_sessionId;

// User override for audit directory (empty => default)
static QString  g_auditDirOverride;

// For performance we keep one open file handle per day.
// If day changes, we close + reopen.
static QFile*   g_auditFile = nullptr;
static QString  g_openDate;     // "yyyy-MM-dd"
static QString  g_openPath;     // current file path

// =====================================================
// Helpers
// =====================================================

static QString isoNowWithMs()
{
    // Example: 2025-12-21T03:12:44.123+02:00
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

static QString dayKey()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd");
}

static QString defaultAuditDir()
{
    // Default: ~/.local/share/<AppName>/audit
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir::cleanPath(root + "/audit");
}

static QString baseAuditDirLocked()
{
    // IMPORTANT: must be called with g_auditMutex held
    const QString ov = g_auditDirOverride.trimmed();
    if (!ov.isEmpty())
        return QDir::cleanPath(ov);
    return defaultAuditDir();
}

static QString filePathForDayLocked(const QString& day)
{
    // audit-YYYY-MM-DD.jsonl
    return QDir(baseAuditDirLocked()).filePath(QString("audit-%1.jsonl").arg(day));
}

static void closeAuditFileLocked()
{
    // IMPORTANT: must be called with g_auditMutex held
    if (g_auditFile) {
        if (g_auditFile->isOpen())
            g_auditFile->close();
        delete g_auditFile;
        g_auditFile = nullptr;
    }
    g_openDate.clear();
    g_openPath.clear();
}

static bool ensureOpenLocked(QString* err)
{
    // IMPORTANT: must be called with g_auditMutex held
    if (err) err->clear();

    const QString today = dayKey();
    const QString wantPath = filePathForDayLocked(today);

    if (g_auditFile && g_auditFile->isOpen() && g_openDate == today && g_openPath == wantPath) {
        return true;
    }

    // Ensure directory exists
    QDir().mkpath(baseAuditDirLocked());

    // Close previous (if day changed or dir changed)
    closeAuditFileLocked();

    g_auditFile = new QFile(wantPath);
    if (!g_auditFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (err) *err = QString("AuditLogger: failed to open %1: %2")
                            .arg(wantPath, g_auditFile->errorString());
        closeAuditFileLocked();
        return false;
    }

    g_openDate = today;
    g_openPath = wantPath;
    return true;
}

static QString hashUtf8Short(const QString& s)
{
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(h.left(16));
}

// =====================================================
// Public API
// =====================================================

namespace AuditLogger {

void install(const QString& appName)
{
    QMutexLocker lock(&g_auditMutex);
    g_appName = appName;

    QString err;
    ensureOpenLocked(&err);
    // No qInfo/qWarning here to avoid recursion with Logger.
}

void setSessionId(const QString& sessionId)
{
    QMutexLocker lock(&g_auditMutex);
    g_sessionId = sessionId;
}

QString sessionId()
{
    QMutexLocker lock(&g_auditMutex);
    return g_sessionId;
}

void setAuditDirOverride(const QString& absoluteDirPath)
{
    QMutexLocker lock(&g_auditMutex);

    const QString newVal = QDir::cleanPath(absoluteDirPath.trimmed());
    if (newVal == g_auditDirOverride)
        return;

    g_auditDirOverride = newVal;

    // Force reopen on next write (and update g_openPath immediately)
    QString err;
    ensureOpenLocked(&err);
}

QString auditDirOverride()
{
    QMutexLocker lock(&g_auditMutex);
    return g_auditDirOverride;
}

QString auditDir()
{
    QMutexLocker lock(&g_auditMutex);
    return baseAuditDirLocked();
}

QString currentLogFilePath()
{
    QMutexLocker lock(&g_auditMutex);
    if (!g_openPath.isEmpty())
        return g_openPath;
    return filePathForDayLocked(dayKey());
}

void writeEvent(const QString& eventName, const QJsonObject& fields)
{
    QMutexLocker lock(&g_auditMutex);

    QString err;
    if (!ensureOpenLocked(&err)) {
        // No file => no audit output. Keep quiet by design.
        return;
    }

    QJsonObject o;
    o.insert("ts", isoNowWithMs());
    o.insert("event", eventName);

    o.insert("app", g_appName.isEmpty() ? QCoreApplication::applicationName() : g_appName);
    o.insert("pid", (int)QCoreApplication::applicationPid());
    o.insert("thread", (qint64)QThread::currentThreadId());

    if (!g_sessionId.isEmpty())
        o.insert("session_id", g_sessionId);

    o.insert("os", QSysInfo::prettyProductName());
    o.insert("arch", QSysInfo::currentCpuArchitecture());

    for (auto it = fields.begin(); it != fields.end(); ++it)
        o.insert(it.key(), it.value());

    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n";
    g_auditFile->write(line);
    g_auditFile->flush();
}

// Optional helper if you want to log a command without storing it.
QJsonObject safeCommandFields(const QString& command, int timeoutMs)
{
    QJsonObject f;
    f.insert("cmd_hash", hashUtf8Short(command));
    f.insert("timeout_ms", timeoutMs);
    const QString first = command.trimmed().section(' ', 0, 0);
    if (!first.isEmpty()) f.insert("cmd_head", first.left(64));
    return f;
}

} // namespace AuditLogger
