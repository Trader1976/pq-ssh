#pragma once
#include <QString>

struct SshProfile {
    QString name;
    QString user;
    QString host;
    int     port    = 22;
    bool    pqDebug = true;

    QString termColorScheme;   // e.g. "WhiteOnBlack"
    int     termFontSize = 11;

    int     termWidth  = 900;
    int     termHeight = 500;
};
