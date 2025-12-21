#include "FleetExecutor.h"

#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>
#include <QUuid>
#include <functional>
#include <QSharedPointer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>



static QString actionToTitle(const FleetAction& a)
{
    switch (a.type) {
        case FleetActionType::RunCommand:     return a.title.isEmpty() ? "Run command" : a.title;
        case FleetActionType::CheckService:   return a.title.isEmpty() ? "Check service" : a.title;
        case FleetActionType::RestartService: return a.title.isEmpty() ? "Restart service" : a.title;
    }
    return a.title;
}

static QString buildCommand(const FleetAction& a)
{
    const QString x = a.payload.trimmed();

    if (a.type == FleetActionType::RunCommand) {
        return x;
    }

    if (a.type == FleetActionType::CheckService) {
        // Keep it simple and predictable
        // Output: active/inactive/failed + exit code reflects state
        return QString("systemctl is-active %1").arg(x);
    }

    if (a.type == FleetActionType::RestartService) {
        // Return status after restart so user sees something useful
        return QString("sudo systemctl restart %1 && systemctl is-active %1").arg(x);
    }

    return x;
}

FleetExecutor::FleetExecutor(QObject* parent)
    : QObject(parent)
{
}

void FleetExecutor::setMaxConcurrency(int n)
{
    m_maxConcurrency = qBound(1, n, 32);
}

void FleetExecutor::clearWatchers()
{
    for (auto& w : m_watchers) {
        if (w) {
            w->disconnect(this);
            w->deleteLater();
        }
    }
    m_watchers.clear();
}

void FleetExecutor::cancel()
{
    m_cancelRequested.storeRelease(1);
}

void FleetExecutor::start(const QVector<SshProfile>& profiles,
                          const QVector<int>& profileIndexes,
                          const FleetAction& action)
{
    if (m_running) return;

    m_running = true;
    m_cancelRequested.storeRelease(0);

    clearWatchers();

    // Snapshot profiles so jobs don't change mid-flight
    m_profilesSnapshot = profiles;

    // Init job
    m_job = FleetJob{};
    m_job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_job.title = actionToTitle(action);
    m_job.profileIndexes = profileIndexes;
    m_job.action = action;
    m_job.startedAt = QDateTime::currentDateTime();
    m_job.results.clear();

    m_total = profileIndexes.size();
    m_done  = 0;

    // Set global pool concurrency (QtConcurrent uses global pool by default)
    QThreadPool::globalInstance()->setMaxThreadCount(m_maxConcurrency);

    emit jobStarted(m_job);

    // Nothing to do
    if (profileIndexes.isEmpty()) {
        m_running = false;
        emit jobFinished(m_job);
        return;
    }

    // Strategy:
    // - split targets into chunks of size = maxConcurrency
    // - run each chunk in parallel (single QtConcurrent::run per chunk)
    // - when chunk finishes, append results, emit progress, start next chunk
    const int chunkSize = qMax(1, m_maxConcurrency);

    // Cursor must survive async callbacks -> heap it
    auto cursor = QSharedPointer<int>::create(0);

    // Self-referential callable that survives signal callbacks safely
    auto startNextChunk = QSharedPointer<std::function<void()>>::create();

    *startNextChunk = [this, cursor, startNextChunk, profileIndexes, chunkSize]() mutable {

        // If cancel requested before scheduling, mark remaining quickly and finish
        if (m_cancelRequested.loadAcquire() != 0) {
            while (*cursor < profileIndexes.size()) {
                const int profileIndex = profileIndexes[*cursor];
                (*cursor)++;

                FleetTargetResult r;
                r.profileIndex = profileIndex;

                if (profileIndex >= 0 && profileIndex < m_profilesSnapshot.size()) {
                    const auto& p = m_profilesSnapshot[profileIndex];
                    r.profileName = p.name;
                    r.group = p.group;
                    r.user = p.user;
                    r.host = p.host;
                    r.port = (p.port > 0) ? p.port : 22;
                }

                r.state = FleetTargetState::Canceled;
                r.error = "Canceled";
                m_job.results.push_back(r);
                m_done++;
                emit jobProgress(m_job, m_done, m_total);
            }

            m_running = false;
            emit jobFinished(m_job);
            return;
        }

        // Done?
        if (*cursor >= profileIndexes.size()) {
            m_running = false;
            emit jobFinished(m_job);
            return;
        }

        // Build next chunk
        QVector<int> chunk;
        chunk.reserve(chunkSize);

        for (int i = 0; i < chunkSize && *cursor < profileIndexes.size(); ++i) {
            chunk.push_back(profileIndexes[*cursor]);
            (*cursor)++;
        }

        // Watcher per chunk (simplest + safe)
        using ChunkResult = QVector<FleetTargetResult>;
        auto *watcher = new QFutureWatcher<ChunkResult>(this);
        m_watchers.push_back(watcher);

        // When chunk finishes -> collect results -> schedule next chunk
        QObject::connect(watcher, &QFutureWatcher<ChunkResult>::finished,
                         this, [this, watcher, startNextChunk]() mutable {

            ChunkResult results;
            // QFuture<QVector<T>> -> result() gives the QVector<T>
            results = watcher->future().result();

            // Append + progress
            for (const auto& r : results) {
                m_job.results.push_back(r);
                m_done++;
            }
            emit jobProgress(m_job, m_done, m_total);

            // Cleanup watcher
            watcher->deleteLater();

            // Continue
            (*startNextChunk)();
        });

        // Run chunk
        watcher->setFuture(QtConcurrent::run([this, chunk]() -> ChunkResult {
            ChunkResult out;
            out.reserve(chunk.size());

            for (int profileIndex : chunk) {

                if (m_cancelRequested.loadAcquire() != 0) {
                    // Mark remaining as canceled quickly
                    FleetTargetResult r;
                    r.profileIndex = profileIndex;

                    if (profileIndex >= 0 && profileIndex < m_profilesSnapshot.size()) {
                        const auto& p = m_profilesSnapshot[profileIndex];
                        r.profileName = p.name;
                        r.group = p.group;
                        r.user = p.user;
                        r.host = p.host;
                        r.port = (p.port > 0) ? p.port : 22;
                    }

                    r.state = FleetTargetState::Canceled;
                    r.error = "Canceled";
                    out.push_back(r);
                    continue;
                }

                if (profileIndex < 0 || profileIndex >= m_profilesSnapshot.size()) {
                    FleetTargetResult r;
                    r.profileIndex = profileIndex;
                    r.state = FleetTargetState::Failed;
                    r.error = "Invalid profile index";
                    out.push_back(r);
                    continue;
                }

                out.push_back(runOneTarget(m_profilesSnapshot[profileIndex],
                                           profileIndex,
                                           m_job.action));
            }

            return out;
        }));
    };

    // Kick off first chunk
    (*startNextChunk)();
}


FleetTargetResult FleetExecutor::runOneTarget(const SshProfile& p,
                                             int profileIndex,
                                             const FleetAction& action)
{
    FleetTargetResult r;
    r.profileIndex = profileIndex;
    r.profileName  = p.name;
    r.group        = p.group;
    r.user         = p.user;
    r.host         = p.host;
    r.port         = (p.port > 0) ? p.port : 22;

    if (m_cancelRequested.loadAcquire() != 0) {
        r.state = FleetTargetState::Canceled;
        r.error = "Canceled";
        return r;
    }

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        r.state = FleetTargetState::Failed;
        r.error = "Empty user/host";
        return r;
    }

    const QString cmd = buildCommand(action);
    if (cmd.trimmed().isEmpty()) {
        r.state = FleetTargetState::Failed;
        r.error = "Empty command/service";
        return r;
    }

    QElapsedTimer t;
    t.start();

    // One client per target (critical!)
    SshClient client;
    QString err;

    if (!client.connectProfile(p, &err)) {
        r.state = (m_cancelRequested.loadAcquire() != 0) ? FleetTargetState::Canceled : FleetTargetState::Failed;
        r.error = err;
        r.durationMs = t.elapsed();
        return r;
    }

    QString out, e;

    // ------------------------------------------------------------
    // Fleet policy: command timeout
    //
    // Pick one:
    //   A) a member like m_commandTimeoutMs
    //   B) a constant for now
    // ------------------------------------------------------------
    const int timeoutMs =
        (m_commandTimeoutMs > 0) ? m_commandTimeoutMs : (90 * 1000); // default 90s

    // IMPORTANT: this requires your SshClient overload:
    // bool exec(const QString& command, QString* out, QString* err, int timeoutMs);
    const bool ok = client.exec(cmd, &out, &e, timeoutMs);

    client.disconnect();

    r.durationMs = t.elapsed();

    if (m_cancelRequested.loadAcquire() != 0) {
        r.state = FleetTargetState::Canceled;
        r.error = "Canceled";
        return r;
    }

    r.stdoutText = out;
    r.stderrText = e;

    if (!ok) {
        // If your exec() sets a recognizable message on timeout, you can map it:
        const QString trimmed = e.trimmed();
        if (trimmed.contains("timed out", Qt::CaseInsensitive) ||
            trimmed.contains("timeout", Qt::CaseInsensitive)) {
            r.state = FleetTargetState::Failed; // or introduce FleetTargetState::Timeout later
            r.error = QString("Timeout after %1 ms").arg(timeoutMs);
        } else {
            r.state = FleetTargetState::Failed;
            r.error = trimmed.isEmpty() ? "Command failed" : trimmed;
        }
        return r;
    }

    r.state = FleetTargetState::Ok;
    return r;
}
