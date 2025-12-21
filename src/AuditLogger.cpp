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
static QString  g_auditDirOverride;

// For performance we keep one open file handle per day.
// If day changes, we close + reopen.
static QFile*   g_auditFile = nullptr;
static QString  g_openDate;     // "yyyy-MM-dd"
static QString  g_openPath;     // current file path


static QString isoNowWithMs()
{
    // Example: 2025-12-21T03:12:44.123+02:00
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}


static QString dayKey()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd");
}

static QString baseAuditDir()
{
    // Override wins if set
    if (!g_auditDirOverride.trimmed().isEmpty())
        return QDir::cleanPath(g_auditDirOverride.trimmed());

    // Default:
    // ~/.local/share/<AppName>/audit
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return root + "/audit";
}

static QString filePathForDay(const QString& day)
{
    // audit-YYYY-MM-DD.jsonl
    return baseAuditDir() + QString("/audit-%1.jsonl").arg(day);
}

static bool ensureOpenLocked(QString* err)
{
    if (err) err->clear();

    const QString today = dayKey();
    const QString wantPath = filePathForDay(today);

    if (g_auditFile && g_auditFile->isOpen() && g_openDate == today && g_openPath == wantPath) {
        return true;
    }

    // Ensure directory exists
    QDir().mkpath(baseAuditDir());

    // Close previous
    if (g_auditFile) {
        if (g_auditFile->isOpen())
            g_auditFile->close();
        delete g_auditFile;
        g_auditFile = nullptr;
    }

    g_auditFile = new QFile(wantPath);
    if (!g_auditFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (err) *err = QString("AuditLogger: failed to open %1: %2")
                            .arg(wantPath, g_auditFile->errorString());
        delete g_auditFile;
        g_auditFile = nullptr;
        g_openDate.clear();
        g_openPath.clear();
        return false;
    }

    g_openDate = today;
    g_openPath = wantPath;
    return true;
}

static QString hashUtf8Short(const QString& s)
{
    // Useful for commands/payloads without storing the raw content
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(h.left(16)); // short prefix is enough for correlation
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

    // We do NOT use qInfo/qWarning here to avoid recursion / mixing with Logger.
    // If open fails, we silently continue; writeEvent will no-op until fixed.
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

QString auditDir()
{
    return baseAuditDir();
}

QString currentLogFilePath()
{
    QMutexLocker lock(&g_auditMutex);
    return g_openPath.isEmpty() ? filePathForDay(dayKey()) : g_openPath;
}

void writeEvent(const QString& eventName, const QJsonObject& fields)
{
    // Thread-safe + safe for QtConcurrent worker threads.
    QMutexLocker lock(&g_auditMutex);

    QString err;
    if (!ensureOpenLocked(&err)) {
        // No file => no audit output. (Could fallback to stderr, but keep quiet for now.)
        return;
    }

    // Build base envelope
    QJsonObject o;
    o.insert("ts", isoNowWithMs());
    o.insert("event", eventName);

    // Useful process/app context
    o.insert("app", g_appName.isEmpty() ? QCoreApplication::applicationName() : g_appName);
    o.insert("pid", (int)QCoreApplication::applicationPid());
    o.insert("thread", (qint64)QThread::currentThreadId());

    if (!g_sessionId.isEmpty())
        o.insert("session_id", g_sessionId);

    // Stable machine-ish info (not too invasive)
    // (If you feel this is too much, remove these 2 lines.)
    o.insert("os", QSysInfo::prettyProductName());
    o.insert("arch", QSysInfo::currentCpuArchitecture());

    // Merge caller-provided fields
    for (auto it = fields.begin(); it != fields.end(); ++it)
        o.insert(it.key(), it.value());

    // Write JSONL
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
    // If you want *some* visibility without the full command, store first token only:
    const QString first = command.trimmed().section(' ', 0, 0);
    if (!first.isEmpty()) f.insert("cmd_head", first.left(64));
    return f;
}

}

void AuditLogger::setAuditDirOverride(const QString& absoluteDir)
{
    QMutexLocker lock(&g_auditMutex);

    const QString dir = QDir::cleanPath(absoluteDir.trimmed());
    g_auditDirOverride = dir;

    // Force reopen on next write (and close current)
    if (g_auditFile) {
        if (g_auditFile->isOpen())
            g_auditFile->close();
        delete g_auditFile;
        g_auditFile = nullptr;
    }
    g_openDate.clear();
    g_openPath.clear();

    // Best effort create dir now (optional)
    if (!g_auditDirOverride.isEmpty())
        QDir().mkpath(g_auditDirOverride);
}
