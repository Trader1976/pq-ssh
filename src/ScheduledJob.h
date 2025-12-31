#pragma once

#include <QString>
#include <QDateTime>
#include <QUuid>

struct ScheduledJob
{
    QString id;
    QString name;
    QString profileId;
    QString command;
    bool enabled = true;

    enum class Kind { OneShot, Recurring };
    Kind kind = Kind::OneShot;

    QDateTime runAtLocal;
    QString onCalendar;

    bool installed = false;
    QString lastInstallError;

    // Legacy (old schema): migrate profile_index -> profile_id after profiles load
    int legacyProfileIndex = -1;
};

static inline QString newJobId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
