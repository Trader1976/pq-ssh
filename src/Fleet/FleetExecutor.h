#pragma once

#include <QObject>
#include <QVector>
#include <QAtomicInteger>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QPointer>
#include <QSpinBox>

#include "FleetTypes.h"
#include "../SshClient.h"
#include "../ProfileStore.h" // for SshProfile

class FleetExecutor : public QObject
{
    Q_OBJECT
public:
    explicit FleetExecutor(QObject* parent = nullptr);

    void setMaxConcurrency(int n);  // default 4
    int  maxConcurrency() const { return m_maxConcurrency; }

    void setCommandTimeoutMs(int ms) { m_commandTimeoutMs = ms; } // ms <= 0 => default
    int  commandTimeoutMs() const { return m_commandTimeoutMs; }

    bool isRunning() const { return m_running; }

    // Start a job. Profiles are passed by value (safe snapshot).
    void start(const QVector<SshProfile>& profiles,
               const QVector<int>& profileIndexes,
               const FleetAction& action);

    void cancel();

    signals:
        void jobStarted(const FleetJob& job);
    void jobProgress(const FleetJob& job, int done, int total);
    void jobFinished(const FleetJob& job);

private:
    FleetTargetResult runOneTarget(const SshProfile& p, int profileIndex, const FleetAction& action);

    int m_commandTimeoutMs = 90 * 1000; // default 90s (can be overridden by UI)
    int m_maxConcurrency   = 4;

    bool m_running = false;
    QAtomicInteger<int> m_cancelRequested { 0 };

    QVector<SshProfile> m_profilesSnapshot;
    FleetJob m_job;

    // One watcher per chunk (simple concurrency without custom queue)
    QVector<QPointer<QFutureWatcher<QVector<FleetTargetResult>>>> m_watchers;

    int m_total = 0;
    int m_done  = 0;

    void clearWatchers();

    QSpinBox* m_timeoutSpin = nullptr;
};
