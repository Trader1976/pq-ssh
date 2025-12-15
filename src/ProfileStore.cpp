#include "ProfileStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QString ProfileStore::configPath()
{
    // Profiles live inside the project directory now:
    // pq-ssh/profiles/profiles.json

    QString baseDir = QCoreApplication::applicationDirPath();

    // If binary is in build/bin/, go up two levels to project root
    QDir dir(baseDir);
    dir.cdUp(); // bin -> build
    dir.cdUp(); // build -> pq-ssh

    const QString profilesDir = dir.absolutePath() + "/profiles";
    if (!QDir().exists(profilesDir)) {
        QDir().mkpath(profilesDir);
    }

    return profilesDir + "/profiles.json";
}

QVector<SshProfile> ProfileStore::defaults()
{
    QVector<SshProfile> out;

    const QString user = qEnvironmentVariable("USER", "user");

    SshProfile p;
    p.name    = "Localhost";
    p.user    = user;
    p.host    = "localhost";
    p.port    = 22;
    p.pqDebug = true;

    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize    = 11;
    p.termWidth       = 900;
    p.termHeight      = 500;

    // ✅ NEW: terminal scrollback history (0 = unlimited)
    p.historyLines    = 2000;

    // Key auth defaults (empty keyFile means "not set")
    p.keyFile = "";
    p.keyType = "auto";

    out.push_back(p);
    return out;
}

bool ProfileStore::save(const QVector<SshProfile>& profiles, QString* err)
{
    QJsonArray arr;
    for (const auto &prof : profiles) {
        QJsonObject obj;
        obj["name"]     = prof.name;
        obj["user"]     = prof.user;
        obj["host"]     = prof.host;
        obj["port"]     = prof.port;
        obj["pq_debug"] = prof.pqDebug;

        obj["term_color_scheme"] = prof.termColorScheme;
        obj["term_font_size"]    = prof.termFontSize;
        obj["term_width"]        = prof.termWidth;
        obj["term_height"]       = prof.termHeight;

        // ✅ NEW: scrollback history (0 = unlimited)
        obj["history_lines"]     = prof.historyLines;

        // Key-based auth (optional)
        if (!prof.keyFile.trimmed().isEmpty())
            obj["key_file"] = prof.keyFile;

        // Always store key_type (so we can evolve behavior later cleanly)
        // If you prefer, you can omit when "auto", but storing is harmless.
        obj["key_type"] = prof.keyType.trimmed().isEmpty() ? QString("auto") : prof.keyType;

        arr.append(obj);
    }

    QJsonObject root;
    root["profiles"] = arr;

    QJsonDocument doc(root);

    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QString("Could not write profiles.json: %1").arg(f.errorString());
        return false;
    }

    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();

    if (err) err->clear();
    return true;
}

QVector<SshProfile> ProfileStore::load(QString* err)
{
    QVector<SshProfile> out;

    QFile f(configPath());
    if (!f.exists()) {
        if (err) err->clear(); // not an error; caller may seed defaults
        return out;
    }

    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QString("Could not open profiles.json: %1").arg(f.errorString());
        return out;
    }

    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QString("Invalid JSON in profiles.json: %1").arg(perr.errorString());
        return out;
    }

    const QJsonObject root = doc.object();
    const QJsonArray arr = root.value("profiles").toArray();

    for (const QJsonValue &val : arr) {
        if (!val.isObject())
            continue;

        const QJsonObject obj = val.toObject();

        SshProfile p;
        p.name    = obj.value("name").toString();
        p.user    = obj.value("user").toString();
        p.host    = obj.value("host").toString();
        p.port    = obj.value("port").toInt(22);
        p.pqDebug = obj.value("pq_debug").toBool(true);

        p.termColorScheme = obj.value("term_color_scheme").toString("WhiteOnBlack");
        p.termFontSize    = obj.value("term_font_size").toInt(11);
        p.termWidth       = obj.value("term_width").toInt(900);
        p.termHeight      = obj.value("term_height").toInt(500);

        // ✅ NEW: scrollback history (0 = unlimited)
        p.historyLines    = obj.value("history_lines").toInt(2000);

        // Key-based auth (optional)
        p.keyFile = obj.value("key_file").toString();
        p.keyType = obj.value("key_type").toString("auto").trimmed();
        if (p.keyType.isEmpty())
            p.keyType = "auto";

        if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty())
            continue;

        if (p.name.trimmed().isEmpty())
            p.name = QString("%1@%2").arg(p.user, p.host);

        out.push_back(p);
    }

    if (err) err->clear();
    return out;
}
