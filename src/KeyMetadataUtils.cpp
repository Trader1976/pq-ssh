#include "KeyMetadataUtils.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

// ---------- helpers ----------

bool parseIsoUtc(const QString &s, QDateTime &outUtc)
{
    if (s.trimmed().isEmpty()) return false;

    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(s, Qt::ISODate);

    if (!dt.isValid()) return false;
    outUtc = dt.toUTC();
    return true;
}

// ---------- auto-expire ----------

bool autoExpireMetadataFile(const QString &path, QString *errOut, int *expiredSetOut)
{
    if (expiredSetOut) *expiredSetOut = 0;

    QFile f(path);
    if (!f.exists()) return true;

    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "Cannot read " + path;
        return false;
    }

    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return true;

    QJsonObject root = doc.object();
    bool changed = false;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject()) continue;

        QJsonObject meta = it.value().toObject();
        if (meta.value("status").toString() == "revoked") continue;
        if (!meta.value("expire_date").isString()) continue;

        const QString expiresIso = meta.value("expire_date").toString();
        QDateTime expUtc;
        if (!parseIsoUtc(expiresIso, expUtc)) continue;

        if (expUtc < nowUtc && meta.value("status").toString() != "expired") {
            meta["status"] = "expired";
            root[it.key()] = meta;
            changed = true;
            if (expiredSetOut) (*expiredSetOut)++;
        }
    }

    if (!changed) return true;

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = "Cannot write " + path;
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    return true;
}

// ---------- count expired ----------

int countExpiredKeysInMetadata(QString *errOut)
{
    const QString path =
        QDir(QDir::homePath()).filePath(".pq-ssh/keys/metadata.json");

    QFile f(path);
    if (!f.exists()) return 0;

    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "Cannot read " + path;
        return 0;
    }

    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return 0;

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    int count = 0;

    for (const auto &v : doc.object()) {
        if (!v.isObject()) continue;
        const QJsonObject meta = v.toObject();
        if (!meta.value("expire_date").isString()) continue;

        QDateTime expUtc;
        if (parseIsoUtc(meta.value("expire_date").toString(), expUtc) &&
            expUtc < nowUtc)
            count++;
    }
    return count;
}
