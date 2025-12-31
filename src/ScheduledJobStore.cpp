#include "ScheduledJobStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

static QJsonObject jobToJson(const ScheduledJob& j)
{
    QJsonObject o;
    o["id"] = j.id;
    o["name"] = j.name;
    o["profile_id"] = j.profileId.trimmed();
    o["command"] = j.command;
    o["enabled"] = j.enabled;
    o["kind"] = (j.kind == ScheduledJob::Kind::OneShot) ? "one_shot" : "recurring";

    if (j.kind == ScheduledJob::Kind::OneShot) {
        // store ISO with offset for portability
        o["run_at"] = j.runAtLocal.toString(Qt::ISODate);
    } else {
        o["on_calendar"] = j.onCalendar.trimmed();
    }

    // Best-effort install hints
    o["installed"] = j.installed;
    if (!j.lastInstallError.trimmed().isEmpty())
        o["last_install_error"] = j.lastInstallError;

    // Legacy compatibility: if profile_id is still empty but we have legacy index,
    // preserve it so we don't lose the ability to migrate later.
    if (o.value("profile_id").toString().isEmpty() && j.legacyProfileIndex >= 0)
        o["profile_index"] = j.legacyProfileIndex;

    return o;
}

static ScheduledJob jobFromJson(const QJsonObject& o)
{
    ScheduledJob j;

    j.id = o.value("id").toString().trimmed();
    if (j.id.isEmpty())
        j.id = newJobId();

    j.name = o.value("name").toString();
    j.profileId = o.value("profile_id").toString().trimmed();

    // Legacy read (old schema)
    j.legacyProfileIndex = o.value("profile_index").toInt(-1);

    j.command = o.value("command").toString();
    j.enabled = o.value("enabled").toBool(true);

    const QString kind = o.value("kind").toString("one_shot").toLower().trimmed();
    j.kind = (kind == "recurring") ? ScheduledJob::Kind::Recurring : ScheduledJob::Kind::OneShot;

    if (j.kind == ScheduledJob::Kind::OneShot) {
        j.runAtLocal = QDateTime::fromString(o.value("run_at").toString(), Qt::ISODate);
        if (!j.runAtLocal.isValid())
            j.runAtLocal = QDateTime();
        j.onCalendar.clear();
    } else {
        j.onCalendar = o.value("on_calendar").toString();
        j.runAtLocal = QDateTime();
    }

    j.installed = o.value("installed").toBool(false);
    j.lastInstallError = o.value("last_install_error").toString();

    return j;
}

QString ScheduledJobStore::configPath()
{
    QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (cfgDir.trimmed().isEmpty())
        cfgDir = QDir(QDir::homePath()).filePath(".config/CPUNK/pq-ssh");

    QDir().mkpath(cfgDir);
    return QDir(cfgDir).filePath("scheduled-jobs.json");
}

QVector<ScheduledJob> ScheduledJobStore::load(QString* err)
{
    QVector<ScheduledJob> out;

    QFile f(configPath());
    if (!f.exists()) {
        if (err) err->clear();
        return out;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QCoreApplication::translate("ScheduledJobStore",
                                                    "Could not open scheduled-jobs.json: %1")
                            .arg(f.errorString());
        return out;
    }

    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QCoreApplication::translate("ScheduledJobStore",
                                                    "Invalid JSON in scheduled-jobs.json: %1")
                            .arg(perr.errorString());
        return out;
    }

    const QJsonArray arr = doc.object().value("jobs").toArray();
    out.reserve(arr.size());

    for (const auto& v : arr) {
        if (!v.isObject()) continue;
        ScheduledJob j = jobFromJson(v.toObject());
        if (j.command.trimmed().isEmpty()) continue; // skip empty
        out.push_back(j);
    }

    if (err) err->clear();
    return out;
}

bool ScheduledJobStore::save(const QVector<ScheduledJob>& jobs, QString* err)
{
    QJsonArray arr;
    for (const auto& j : jobs) {
        if (j.command.trimmed().isEmpty()) continue;
        arr.append(jobToJson(j));
    }

    QJsonObject root;
    root["format"] = 1;
    root["jobs"] = arr;

    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QCoreApplication::translate("ScheduledJobStore",
                                                    "Could not write scheduled-jobs.json: %1")
                            .arg(f.errorString());
        return false;
    }

    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = QCoreApplication::translate("ScheduledJobStore",
                                                    "Could not write scheduled-jobs.json: %1")
                            .arg(f.errorString());
        return false;
    }

    if (err) err->clear();
    return true;
}
