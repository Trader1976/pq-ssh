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
    // Backward-compat (OLD: single)
    // Keep for now so old profiles.json + older code still compile.
    // Later, remove these after ProfileStore migrates fully.
    // -----------------------------
    QString macroShortcut;   // e.g. "F2" or "Alt+X"
    QString macroCommand;    // command to send
    bool    macroEnter = true;
};
