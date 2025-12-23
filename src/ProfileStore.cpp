// ProfileStore.cpp
//
// ARCHITECTURE NOTES (ProfileStore.cpp)
//
// ProfileStore is the persistence boundary for SSH profiles.
// Responsibilities:
// - Locate config path for profiles.json
// - Serialize/deserialize profiles to JSON
// - Keep backward compatibility with legacy single-macro fields
//
// Non-responsibilities:
// - No UI (dialogs/widgets)
// - No SSH/network operations
//
// Schema evolution:
// - New multi-macro list lives in profile["macros"] (array of objects).
// - Legacy single macro fields ("macro_shortcut", "macro_command", "macro_enter")
//   are still read and written (synced from macros[0]) so older app versions
//   can keep working.
//

#include "ProfileStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>

// -----------------------------
// Helpers: macros <-> JSON
// -----------------------------
static bool isMacroEmpty(const ProfileMacro &m)
{
    return m.name.trimmed().isEmpty()
        && m.shortcut.trimmed().isEmpty()
        && m.command.trimmed().isEmpty();
}

static QJsonObject macroToJson(const ProfileMacro &m)
{
    QJsonObject o;

    const QString nm = m.name.trimmed();
    const QString sc = m.shortcut.trimmed();

    if (!nm.isEmpty()) o["name"] = nm;
    if (!sc.isEmpty()) o["shortcut"] = sc;
    if (!m.command.trimmed().isEmpty()) o["command"] = m.command;

    // Always store for stability (default true)
    o["send_enter"] = m.sendEnter;

    return o;
}

static ProfileMacro macroFromJson(const QJsonObject &o)
{
    ProfileMacro m;
    m.name      = o.value("name").toString();
    m.shortcut  = o.value("shortcut").toString().trimmed();
    m.command   = o.value("command").toString();
    m.sendEnter = o.value("send_enter").toBool(true);
    return m;
}

static QJsonArray macrosToJsonArray(const QVector<ProfileMacro> &macros)
{
    QJsonArray a;
    for (const auto &m : macros) {
        if (isMacroEmpty(m)) continue; // don't persist blank rows
        a.append(macroToJson(m));
    }
    return a;
}

static QVector<ProfileMacro> macrosFromJsonArray(const QJsonArray &a)
{
    QVector<ProfileMacro> out;
    out.reserve(a.size());
    for (const auto &v : a) {
        if (!v.isObject()) continue;
        ProfileMacro m = macroFromJson(v.toObject());
        if (isMacroEmpty(m)) continue;
        out.push_back(m);
    }
    return out;
}

// If macros[] is empty, try migrating legacy single macro fields into macros[0].
static void migrateLegacyMacroToListIfNeeded(SshProfile &p)
{
    if (!p.macros.isEmpty())
        return;

    const QString sc  = p.macroShortcut.trimmed();
    const QString cmd = p.macroCommand.trimmed();

    if (sc.isEmpty() && cmd.isEmpty())
        return;

    ProfileMacro m;
    m.name = "";
    m.shortcut = sc;
    m.command = p.macroCommand;
    m.sendEnter = p.macroEnter;

    p.macros.push_back(m);
}

// Keep backward-compat fields filled from first macro (if present).
static void syncLegacyMacroFromList(SshProfile &p)
{
    if (p.macros.isEmpty())
        return;

    const ProfileMacro &m = p.macros.first();
    p.macroShortcut = m.shortcut.trimmed();
    p.macroCommand  = m.command;
    p.macroEnter    = m.sendEnter;
}

QString ProfileStore::configPath()
{
    QString baseDir = QCoreApplication::applicationDirPath();

    QDir dir(baseDir);
    dir.cdUp(); // bin  -> build
    dir.cdUp(); // build -> pq-ssh (project root)

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
    p.name = QCoreApplication::translate("ProfileStore", "Localhost");
    p.user = user;
    p.host = "localhost";
    p.port = 22;

    // Group (empty => treated as "Ungrouped" in UI)
    p.group = "";

    // Debug defaults
    p.pqDebug = true;

    // Terminal defaults
    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize    = 11;
    p.termWidth       = 900;
    p.termHeight      = 500;
    p.historyLines    = 2000;

    // Auth defaults
    p.keyFile = "";
    p.keyType = "auto";

    // NEW: macros list (start empty; UI can create first row automatically)
    p.macros.clear();

    // Backward-compat defaults (keep consistent)
    p.macroShortcut = "";
    p.macroCommand  = "";
    p.macroEnter    = true;

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
              "group": "...",                // optional
              "user": "...",
              "host": "...",
              "port": 22,
              "pq_debug": true,
              "term_color_scheme": "...",
              "term_font_size": 11,
              "term_width": 900,
              "term_height": 500,
              "history_lines": 2000,
              "key_file": "...",             // optional
              "key_type": "auto",            // always stored

              // NEW: Hotkey macros (multi)
              "macros": [
                { "name": "...", "shortcut": "F2", "command": "cd ...", "send_enter": true }
              ],

              // Backward-compat (OLD: single) - written from macros[0] if present
              "macro_shortcut": "F2",        // optional
              "macro_command":  "cd ...",    // optional
              "macro_enter": true            // always stored (default true)
            }
          ]
        }
    */

    QJsonArray arr;
    for (auto prof : profiles) {
        // Ensure legacy single is in sync with macros[0] (if macros exist)
        syncLegacyMacroFromList(prof);

        QJsonObject obj;

        // Connection identity
        obj["name"] = prof.name;
        obj["user"] = prof.user;
        obj["host"] = prof.host;
        obj["port"] = prof.port;

        // Group (store only if explicitly set)
        if (!prof.group.trimmed().isEmpty())
            obj["group"] = prof.group.trimmed();

        // UI / diagnostics flags
        obj["pq_debug"] = prof.pqDebug;

        // Terminal presentation
        obj["term_color_scheme"] = prof.termColorScheme;
        obj["term_font_size"]    = prof.termFontSize;
        obj["term_width"]        = prof.termWidth;
        obj["term_height"]       = prof.termHeight;
        obj["history_lines"]     = prof.historyLines;

        // Auth
        if (!prof.keyFile.trimmed().isEmpty())
            obj["key_file"] = prof.keyFile;

        obj["key_type"] = prof.keyType.trimmed().isEmpty()
                              ? QStringLiteral("auto")
                              : prof.keyType.trimmed();

        // ---- NEW: Hotkey macros (multi) ----
        // Store only non-empty macros
        const QJsonArray macrosArr = macrosToJsonArray(prof.macros);
        if (!macrosArr.isEmpty())
            obj["macros"] = macrosArr;

        // ---- Backward-compat (OLD: single) ----
        // Write first macro also into legacy keys so older versions still see something.
        const QString sc = prof.macroShortcut.trimmed();
        if (!sc.isEmpty())
            obj["macro_shortcut"] = sc;

        if (!prof.macroCommand.trimmed().isEmpty())
            obj["macro_command"] = prof.macroCommand;

        // Always store for schema stability (default true)
        obj["macro_enter"] = prof.macroEnter;

        arr.append(obj);
    }

    QJsonObject root;
    root["profiles"] = arr;

    QJsonDocument doc(root);

    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QCoreApplication::translate("ProfileStore",
                                                    "Could not write profiles.json: %1")
                            .arg(f.errorString());
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
        if (err) err->clear();
        return out;
    }

    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QCoreApplication::translate("ProfileStore",
                                                    "Could not open profiles.json: %1")
                            .arg(f.errorString());
        return out;
    }

    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QCoreApplication::translate("ProfileStore",
                                                    "Invalid JSON in profiles.json: %1")
                            .arg(perr.errorString());
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
        p.name = obj.value("name").toString();
        p.user = obj.value("user").toString();
        p.host = obj.value("host").toString();
        p.port = obj.value("port").toInt(22);

        // Group
        p.group = obj.value("group").toString().trimmed();

        // Diagnostics / UI flags
        p.pqDebug = obj.value("pq_debug").toBool(true);

        // Terminal defaults if missing
        p.termColorScheme = obj.value("term_color_scheme").toString("WhiteOnBlack");
        p.termFontSize    = obj.value("term_font_size").toInt(11);
        p.termWidth       = obj.value("term_width").toInt(900);
        p.termHeight      = obj.value("term_height").toInt(500);
        p.historyLines    = obj.value("history_lines").toInt(2000);

        // Auth
        p.keyFile = obj.value("key_file").toString();
        p.keyType = obj.value("key_type").toString("auto").trimmed();
        if (p.keyType.isEmpty())
            p.keyType = "auto";

        // ---- NEW: macros (multi) ----
        if (obj.contains("macros") && obj.value("macros").isArray()) {
            p.macros = macrosFromJsonArray(obj.value("macros").toArray());
        } else {
            p.macros.clear();
        }

        // ---- Legacy: single macro (backward compat) ----
        p.macroShortcut = obj.value("macro_shortcut").toString().trimmed();
        p.macroCommand  = obj.value("macro_command").toString();
        p.macroEnter    = obj.value("macro_enter").toBool(true);

        // Migration: if no macros[] present, but legacy single macro exists -> macros[0]
        migrateLegacyMacroToListIfNeeded(p);

        // Also keep legacy fields consistent with macros[0] (if macros exist)
        syncLegacyMacroFromList(p);

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
