#pragma once
#include <QString>

struct SshProfile {
    // Connection
    QString name;
    QString user;
    QString host;
    int     port    = 22;
    bool    pqDebug = true;

    // Terminal
    QString termColorScheme;   // e.g. "WhiteOnBlack"
    int     termFontSize = 11;
    int     termWidth  = 900;
    int     termHeight = 500;
    int historyLines = 2000;
    // Key-based authentication (optional, future PQ-ready)
    // keyType values (suggested):
    //  - "auto"    (default)
    //  - "openssh" (classic OpenSSH keys)
    //  - "pq"      (future PQ keys)
    //  - "mldsa87", "dilithium5", etc (future)
    QString keyFile;           // e.g. /home/timo/.ssh/id_ed25519
    QString keyType = "auto";
};
