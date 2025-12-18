// ProfileStore.cpp

#include "ProfileStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

/*
    ProfileStore
    ------------
    Responsible for loading/saving SSH profiles (JSON-backed).

    Current design (dev-friendly):
    - profiles.json lives inside the *project* folder:
        pq-ssh/profiles/profiles.json

    Notes:
    - This is not yet the typical "installed app config" approach.
      (Installed apps usually store config under QStandardPaths.)
    - For now, this is convenient while iterating locally.
*/

QString ProfileStore::configPath()
{
    /*
        We locate profiles.json relative to the built binary.

        Typical dev layout:
            pq-ssh/
              profiles/profiles.json
              build/
                bin/
                  pq-ssh   <-- applicationDirPath() points here

        So:
            applicationDirPath() = .../pq-ssh/build/bin
            cdUp() -> .../pq-ssh/build
            cdUp() -> .../pq-ssh
    */

    QString baseDir = QCoreApplication::applicationDirPath();

    QDir dir(baseDir);
    dir.cdUp(); // bin  -> build
    dir.cdUp(); // build -> pq-ssh (project root)

    const QString profilesDir = dir.absolutePath() + "/profiles";

    // Ensure folder exists so save() can write successfully
    if (!QDir().exists(profilesDir)) {
        QDir().mkpath(profilesDir);
    }

    return profilesDir + "/profiles.json";
}

QVector<SshProfile> ProfileStore::defaults()
{
    /*
        Seed profile(s) for first run / empty config.
        Caller may use these if load() returns empty.
    */
    QVector<SshProfile> out;

    const QString user = qEnvironmentVariable("USER", "user");

    SshProfile p;
    p.name    = "Localhost";
    p.user    = user;
    p.host    = "localhost";
    p.port    = 22;

    // NEW: group (empty => treated as "Ungrouped" in UI)
    p.group   = "";

    // Debug defaults: currently enabled for localhost so dev logs are visible
    p.pqDebug = true;

    // Terminal UX defaults (feel free to tune as you iterate)
    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize    = 11;
    p.termWidth       = 900;
    p.termHeight      = 500;

    // Scrollback history (0 = unlimited). You default to 2000 lines.
    p.historyLines    = 2000;

    // Key auth defaults:
    // - keyFile empty = "not set"
    // - keyType "auto" means "let libssh/OpenSSH defaults decide"
    p.keyFile = "";
    p.keyType = "auto";

    out.push_back(p);
    return out;
}

bool ProfileStore::save(const QVector<SshProfile>& profiles, QString* err)
{
    /*
        JSON format:
        {
          "profiles": [
            {
              "name": "...",
              "group": "...",           // optional; empty/omitted means "Ungrouped"
              "user": "...",
              "host": "...",
              "port": 22,
              "pq_debug": true,
              "term_color_scheme": "...",
              "term_font_size": 11,
              "term_width": 900,
              "term_height": 500,
              "history_lines": 2000,
              "key_file": "...",        // optional
              "key_type": "auto"        // always stored
            },
            ...
          ]
        }
    */

    QJsonArray arr;
    for (const auto &prof : profiles) {
        QJsonObject obj;

        // Connection identity
        obj["name"]     = prof.name;
        obj["user"]     = prof.user;
        obj["host"]     = prof.host;
        obj["port"]     = prof.port;

        // NEW: group (store only if explicitly set; empty => "Ungrouped")
        if (!prof.group.trimmed().isEmpty())
            obj["group"] = prof.group.trimmed();

        // UI / diagnostics flags
        obj["pq_debug"] = prof.pqDebug;

        // Terminal presentation
        obj["term_color_scheme"] = prof.termColorScheme;
        obj["term_font_size"]    = prof.termFontSize;
        obj["term_width"]        = prof.termWidth;
        obj["term_height"]       = prof.termHeight;

        // Scrollback history lines (0 = unlimited)
        obj["history_lines"]     = prof.historyLines;

        // Key-based auth: store key_file only if explicitly set
        if (!prof.keyFile.trimmed().isEmpty())
            obj["key_file"] = prof.keyFile;

        // Always store key_type to keep schema stable as features evolve
        // (Even if "auto" today, future versions can interpret additional values.)
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
    /*
        Load profiles from configPath().

        Behavior:
        - If file doesn't exist -> returns empty list (not an error).
        - If JSON invalid -> returns empty list and sets err.
        - Invalid profiles (missing host/user) are skipped.
    */

    QVector<SshProfile> out;

    QFile f(configPath());
    if (!f.exists()) {
        if (err) err->clear(); // not an error; caller may seed defaults()
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

        // Connection identity
        p.name    = obj.value("name").toString();
        p.user    = obj.value("user").toString();
        p.host    = obj.value("host").toString();
        p.port    = obj.value("port").toInt(22);

        // NEW: group (missing/empty => treated as "Ungrouped" by UI)
        p.group   = obj.value("group").toString().trimmed();

        // Diagnostics / UI flags
        p.pqDebug = obj.value("pq_debug").toBool(true);

        // Terminal defaults if missing
        p.termColorScheme = obj.value("term_color_scheme").toString("WhiteOnBlack");
        p.termFontSize    = obj.value("term_font_size").toInt(11);
        p.termWidth       = obj.value("term_width").toInt(900);
        p.termHeight      = obj.value("term_height").toInt(500);

        // Scrollback defaults if missing
        p.historyLines    = obj.value("history_lines").toInt(2000);

        // Key auth fields (optional but supported)
        p.keyFile = obj.value("key_file").toString();
        p.keyType = obj.value("key_type").toString("auto").trimmed();
        if (p.keyType.isEmpty())
            p.keyType = "auto";

        // Skip incomplete profiles
        if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty())
            continue;

        // If name missing, generate something human-readable
        if (p.name.trimmed().isEmpty())
            p.name = QString("%1@%2").arg(p.user, p.host);

        out.push_back(p);
    }

    if (err) err->clear();
    return out;
}
