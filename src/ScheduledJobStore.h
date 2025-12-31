#pragma once

#include <QString>
#include <QVector>
#include "ScheduledJob.h"

class ScheduledJobStore
{
public:
    static QString configPath();                 // .../scheduled-jobs.json
    static QVector<ScheduledJob> load(QString* err = nullptr);
    static bool save(const QVector<ScheduledJob>& jobs, QString* err = nullptr);
};
