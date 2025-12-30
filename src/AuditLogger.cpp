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
#include <QSysInfo>
#include <QThread>
#include <QCryptographicHash>
#include <QCoreApplication>


// =====================================================
// Global audit logger state (process-wide)
// =====================================================
//
// This module is designed as a process-wide singleton (no instance object).
// It uses a single mutex to guard all state and a single open file handle,
// rotated by day ("audit-YYYY-MM-DD.jsonl").
//
// Design intent:
// - Append-only JSONL: one JSON object per line (easy to stream, grep, parse).
// - No secrets: caller should avoid passing sensitive values in `fields`.
// - Quiet failure: if logging cannot write, it silently drops events.
// =====================================================

static QMutex   g_auditMutex;        // Guards ALL global state below.
static QString  g_appName;           // Optional override for app name in events.
static QString  g_sessionId;         // Optional session identifier to correlate events.

// User override for audit directory (empty => default)
static QString  g_auditDirOverride;

// For performance we keep one open file handle per day.
// If day changes, we close + reopen.
static QFile*   g_auditFile = nullptr;  // Owned by this module (new/delete).
static QString  g_openDate;             // "yyyy-MM-dd" currently opened day.
static QString  g_openPath;             // current file path (derived from day + dir).

// =====================================================
// Helpers
// =====================================================

static QString isoNowWithMs()
{
    // Timestamp with milliseconds and local timezone offset.
    // Example: 2025-12-21T03:12:44.123+02:00
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

static QString dayKey()
{
    // Day "bucket" used for daily file rotation.
    // NOTE: uses local time; if you prefer UTC rotation, use currentDateTimeUtc().
    return QDateTime::currentDateTime().toString("yyyy-MM-dd");
}

static QString defaultAuditDir()
{
    // Default: ~/.local/share/<AppName>/audit   (on Linux)
    // QStandardPaths::AppLocalDataLocation is platform-dependent.
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir::cleanPath(root + "/audit");
}

static QString baseAuditDirLocked()
{
    // IMPORTANT: must be called with g_auditMutex held
    // Return override if set, otherwise platform default.
    const QString ov = g_auditDirOverride.trimmed();
    if (!ov.isEmpty())
        return QDir::cleanPath(ov);
    return defaultAuditDir();
}

static QString filePathForDayLocked(const QString& day)
{
    // IMPORTANT: must be called with g_auditMutex held
    // audit-YYYY-MM-DD.jsonl
    return QDir(baseAuditDirLocked()).filePath(QString("audit-%1.jsonl").arg(day));
}

static void closeAuditFileLocked()
{
    // IMPORTANT: must be called with g_auditMutex held
    // Close and delete current file handle (safe to call multiple times).
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
    //
    // Ensures a writable file is open for "today".
    // If day (or directory) changes, we rotate by closing and reopening.
    //
    // Returns:
    // - true  if g_auditFile is open and ready
    // - false if opening fails (logging will be dropped)
    if (err) err->clear();

    const QString today = dayKey();
    const QString wantPath = filePathForDayLocked(today);

    // Fast path: correct file already open
    if (g_auditFile && g_auditFile->isOpen() && g_openDate == today && g_openPath == wantPath) {
        return true;
    }

    // Ensure directory exists (ignore failure here; open() will fail and report)
    QDir().mkpath(baseAuditDirLocked());

    // Close previous (if day changed or dir changed)
    closeAuditFileLocked();

    g_auditFile = new QFile(wantPath);
    if (!g_auditFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (err) {
            // Use QCoreApplication::translate so message is translatable.
            *err = QCoreApplication::translate(
                       "AuditLogger",
                       "AuditLogger: failed to open %1: %2"
                   ).arg(wantPath, g_auditFile->errorString());
        }
        closeAuditFileLocked();
        return false;
    }

    g_openDate = today;
    g_openPath = wantPath;
    return true;
}

static QString hashUtf8Short(const QString& s)
{
    // Short, non-reversible identifier for potentially sensitive strings.
    // Uses SHA-256 and keeps first 16 hex chars (64 bits of display).
    // Useful for correlating identical commands without storing raw command.
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(h.left(16));
}

// =====================================================
// Public API
// =====================================================

namespace AuditLogger {

void install(const QString& appName)
{
    // Called once at startup (ideally).
    // Stores app name override and attempts to open current log file.
    // NOTE: intentionally no qInfo/qWarning here to avoid recursion
    // if your logging system itself uses AuditLogger.
    QMutexLocker lock(&g_auditMutex);
    g_appName = appName;

    QString err;
    ensureOpenLocked(&err);
}

void setSessionId(const QString& sessionId)
{
    // Session id allows grouping events across app runtime or per-connection.
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
    // Allows user/config to relocate audit logs.
    // Caller should pass an absolute path; we don't enforce absolute here,
    // but storing relative paths could surprise users.
    QMutexLocker lock(&g_auditMutex);

    const QString newVal = QDir::cleanPath(absoluteDirPath.trimmed());
    if (newVal == g_auditDirOverride)
        return;

    g_auditDirOverride = newVal;

    // Force reopen immediately so currentLogFilePath() reflects new dir.
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
    // Returns effective directory (override if set, else default)
    QMutexLocker lock(&g_auditMutex);
    return baseAuditDirLocked();
}

QString currentLogFilePath()
{
    // Returns the path to the file that will be written today.
    // If not opened yet, returns the computed path for today's dayKey.
    QMutexLocker lock(&g_auditMutex);
    if (!g_openPath.isEmpty())
        return g_openPath;
    return filePathForDayLocked(dayKey());
}

void writeEvent(const QString& eventName, const QJsonObject& fields)
{
    // Writes one JSON object as a single line (JSONL).
    //
    // Thread safety:
    // - Global mutex serializes writes and file rotation.
    //
    // Failure behavior:
    // - If ensureOpenLocked fails, we drop the event quietly.
    QMutexLocker lock(&g_auditMutex);

    QString err;
    if (!ensureOpenLocked(&err)) {
        // No file => no audit output. Keep quiet by design.
        return;
    }

    QJsonObject o;
    o.insert("ts", isoNowWithMs());
    o.insert("event", eventName);

    // Common metadata
    o.insert("app", g_appName.isEmpty() ? QCoreApplication::applicationName() : g_appName);
    o.insert("pid", (int)QCoreApplication::applicationPid());
    o.insert("thread", (qint64)QThread::currentThreadId()); // NOTE: pointer-ish value, not stable across runs

    if (!g_sessionId.isEmpty())
        o.insert("session_id", g_sessionId);

    o.insert("os", QSysInfo::prettyProductName());
    o.insert("arch", QSysInfo::currentCpuArchitecture());

    // Merge caller-supplied fields (caller controls what is logged!)
    for (auto it = fields.begin(); it != fields.end(); ++it)
        o.insert(it.key(), it.value());

    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n";

    // NOTE: QFile::write can fail; currently ignored by design.
    g_auditFile->write(line);

    // Flush makes events durable immediately, but costs IO.
    // If performance becomes an issue, consider:
    // - flush every N lines
    // - flush on timer
    // - rely on OS buffering
    g_auditFile->flush();
}

// Optional helper if you want to log a command without storing it.
QJsonObject safeCommandFields(const QString& command, int timeoutMs)
{
    // Logs a short hash (cmd_hash) for correlation and the first token (cmd_head)
    // to give operators a clue ("ls", "cp", "rm", "ssh", etc.) without full command.
    QJsonObject f;
    f.insert("cmd_hash", hashUtf8Short(command));
    f.insert("timeout_ms", timeoutMs);

    const QString first = command.trimmed().section(' ', 0, 0);
    if (!first.isEmpty()) f.insert("cmd_head", first.left(64));

    return f;
}

} // namespace AuditLogger
