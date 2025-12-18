#pragma once

#include <QString>

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

    // Hotkey macro (single)
    QString macroShortcut;   // e.g. "F2" or "Alt+X"
    QString macroCommand;    // command to send
    bool    macroEnter = true;
};
