#pragma once

#include <QVector>
#include <QString>
#include "SshProfile.h"

class ProfileStore {
public:
    static QString configPath();

    // Return profiles; if err != nullptr, set it on error (non-fatal).
    static QVector<SshProfile> load(QString* err = nullptr);

    // Save profiles; returns false on error and sets err if provided.
    static bool save(const QVector<SshProfile>& profiles, QString* err = nullptr);

    // Default profiles to seed a new file
    static QVector<SshProfile> defaults();
};
