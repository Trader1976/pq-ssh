#pragma once

#include <QString>
#include <QVector>

#include "SshProfile.h"   // <-- include, do NOT redefine

class ProfileStore
{
public:
    static QString configPath();
    static QVector<SshProfile> defaults();
    static bool save(const QVector<SshProfile>& profiles, QString* err = nullptr);
    static QVector<SshProfile> load(QString* err = nullptr);
};