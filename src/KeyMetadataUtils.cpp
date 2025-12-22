// KeyMetadataUtils.cpp
#include "KeyMetadataUtils.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QObject>

/*
 * KeyMetadataUtils
 * ----------------
 * Small utility helpers for working with key metadata (metadata.json).
 *
 * Responsibilities:
 *  - Parse ISO-8601 UTC timestamps safely.
 *  - Automatically mark expired keys based on expire_date.
 *  - Provide lightweight queries (e.g. count expired keys).
 *
 * This module is intentionally:
 *  - Stateless
 *  - UI-agnostic
 *  - Independent from SSH / crypto logic
 *
 * It is safe to call from UI refresh paths.
 */

// ===========================================================================
// Helpers
// ===========================================================================

/*
 * parseIsoUtc
 * -----------
 * Parses an ISO-8601 timestamp string and normalizes it to UTC.
 *
 * Accepted formats:
 *  - Qt::ISODateWithMs (preferred)
 *  - Qt::ISODate      (fallback)
 *
 * @param s       Input timestamp string
 * @param outUtc  Output QDateTime (UTC) on success
 * @return        true if parsed successfully
 */
bool parseIsoUtc(const QString &s, QDateTime &outUtc)
{
    if (s.trimmed().isEmpty())
        return false;

    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(s, Qt::ISODate);

    if (!dt.isValid())
        return false;

    outUtc = dt.toUTC();
    return true;
}

// ===========================================================================
// Auto-expire metadata
// ===========================================================================

/*
 * autoExpireMetadataFile
 * ----------------------
 * Scans a metadata.json file and automatically marks keys as "expired"
 * when their expire_date is in the past.
 *
 * Rules:
 *  - Keys with status "revoked" are NOT modified.
 *  - Only keys with a valid expire_date string are considered.
 *  - Status is changed to "expired" only once.
 *
 * This function is intentionally idempotent:
 *  - Calling it repeatedly is safe.
 *  - If nothing changes, the file is not rewritten.
 *
 * Typical usage:
 *  - Called during UI refresh (e.g. KeyGeneratorDialog::refreshKeysTable)
 *
 * @param path             Path to metadata.json
 * @param errOut           Optional error message output
 * @param expiredSetOut    Optional counter for how many keys were marked expired
 * @return                 true on success or no-op, false on hard failure
 */
bool autoExpireMetadataFile(const QString &path, QString *errOut, int *expiredSetOut)
{
    if (expiredSetOut)
        *expiredSetOut = 0;

    QFile f(path);
    if (!f.exists())
        return true; // No metadata yet → nothing to do

    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QObject::tr("Cannot read %1").arg(path);
        return false;
    }

    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject())
        return true; // Corrupt or unexpected format → ignore safely

    QJsonObject root = doc.object();
    bool changed = false;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject())
            continue;

        QJsonObject meta = it.value().toObject();

        // Do not auto-expire revoked keys
        if (meta.value("status").toString() == "revoked")
            continue;

        if (!meta.value("expire_date").isString())
            continue;

        const QString expiresIso = meta.value("expire_date").toString();
        QDateTime expUtc;
        if (!parseIsoUtc(expiresIso, expUtc))
            continue;

        if (expUtc < nowUtc && meta.value("status").toString() != "expired") {
            meta["status"] = "expired";
            root[it.key()] = meta;
            changed = true;
            if (expiredSetOut) (*expiredSetOut)++;
        }
    }

    if (!changed)
        return true; // No changes → avoid touching the file

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = QObject::tr("Cannot write %1").arg(path);
        return false;
    }

    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    return true;
}

// ===========================================================================
// Count expired keys
// ===========================================================================

/*
 * countExpiredKeysInMetadata
 * --------------------------
 * Lightweight helper that counts how many keys in metadata.json
 * are currently expired (based on expire_date vs now).
 *
 * This does NOT modify metadata.json.
 *
 * Typical usage:
 *  - Badge counters
 *  - UI warnings ("You have X expired keys")
 *
 * @param errOut   Optional error output
 * @return         Number of expired keys
 */
int countExpiredKeysInMetadata(QString *errOut)
{
    const QString path =
        QDir(QDir::homePath()).filePath(".pq-ssh/keys/metadata.json");

    QFile f(path);
    if (!f.exists())
        return 0;

    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QObject::tr("Cannot read %1").arg(path);
        return 0;
    }

    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject())
        return 0;

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    int count = 0;

    for (const auto &v : doc.object()) {
        if (!v.isObject())
            continue;

        const QJsonObject meta = v.toObject();
        if (!meta.value("expire_date").isString())
            continue;

        QDateTime expUtc;
        if (parseIsoUtc(meta.value("expire_date").toString(), expUtc) &&
            expUtc < nowUtc)
        {
            count++;
        }
    }

    return count;
}
