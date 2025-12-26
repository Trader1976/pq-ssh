#pragma once

#include <QString>
#include <QVector>

// -----------------------------
// Hotkey macros (multi)
// -----------------------------
struct ProfileMacro {
    QString name;        // optional label (can be empty)
    QString shortcut;    // e.g. "F2" or "Alt+X"
    QString command;     // command to send
    bool    sendEnter = true;
};

// -----------------------------
// Port forwarding (per-profile)
// -----------------------------
enum class PortForwardType {
    Local,   // -L
    Remote,  // -R
    Dynamic  // -D (SOCKS)
};

struct PortForwardRule {
    PortForwardType type = PortForwardType::Local;
    QString bind = "127.0.0.1";   // listen/bind address
    int listenPort = 0;          // required

    // Only for Local/Remote
    QString targetHost = "localhost";
    int targetPort = 0;

    bool enabled = true;
    QString note;
};

static inline QString portForwardTypeToString(PortForwardType t)
{
    switch (t) {
        case PortForwardType::Local:   return "local";
        case PortForwardType::Remote:  return "remote";
        case PortForwardType::Dynamic: return "dynamic";
    }
    return "local";
}

static inline PortForwardType portForwardTypeFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == "remote")  return PortForwardType::Remote;
    if (v == "dynamic") return PortForwardType::Dynamic;
    return PortForwardType::Local;
}

struct SshProfile {
    // Connection
    QString name;
    QString user;
    QString host;
    int     port    = 22;
    bool    pqDebug = true;

    // Grouping
    QString group;

    // Terminal
    QString termColorScheme;
    int     termFontSize  = 11;
    int     termWidth     = 900;
    int     termHeight    = 500;
    int     historyLines  = 2000;

    // Auth
    QString keyFile;
    QString keyType = "auto";

    // -----------------------------
    // Hotkey macros (NEW: multi)
    // -----------------------------
    QVector<ProfileMacro> macros;

    // -----------------------------
    // Port forwarding (NEW)
    // -----------------------------
    bool portForwardingEnabled = false;
    QVector<PortForwardRule> portForwards;

    // -----------------------------
    // Backward-compat (OLD: single)
    // Keep for now so old profiles.json + older code still compile.
    // Later, remove these after ProfileStore migrates fully.
    // -----------------------------
    QString macroShortcut;   // e.g. "F2" or "Alt+X"
    QString macroCommand;    // command to send
    bool    macroEnter = true;
};
