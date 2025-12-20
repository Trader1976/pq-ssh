#pragma once

#include <QString>
#include <QVector>
#include <QDateTime>

enum class FleetActionType {
    RunCommand,
    CheckService,
    RestartService
};

struct FleetAction {
    FleetActionType type = FleetActionType::RunCommand;
    QString title;     // e.g. "Restart nginx"
    QString payload;   // e.g. command string OR service name
};

enum class FleetTargetState {
    Queued,
    Running,
    Ok,
    Failed,
    Canceled
};

struct FleetTargetResult {
    int profileIndex = -1;
    QString profileName;
    QString group;
    QString user;
    QString host;
    int port = 22;

    FleetTargetState state = FleetTargetState::Queued;
    qint64 durationMs = 0;

    QString stdoutText;
    QString stderrText;
    QString error;   // high-level failure reason
};

struct FleetJob {
    QString id;
    QString title;

    QVector<int> profileIndexes; // indices into the profile list you passed to FleetWindow
    FleetAction action;

    QDateTime startedAt;
    QDateTime finishedAt;

    QVector<FleetTargetResult> results;
};
