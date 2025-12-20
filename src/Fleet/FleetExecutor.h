#pragma once

#include <QObject>
#include <QVector>
#include <QAtomicInteger>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QPointer>

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

private:
    int m_maxConcurrency = 4;

    bool m_running = false;
    QAtomicInteger<int> m_cancelRequested { 0 };

    QVector<SshProfile> m_profilesSnapshot;
    FleetJob m_job;

    // One watcher per chunk (so we can implement simple concurrency without writing our own threadpool queue)
    QVector<QPointer<QFutureWatcher<QVector<FleetTargetResult>>>> m_watchers;

    int m_total = 0;
    int m_done  = 0;

    void clearWatchers();
};
