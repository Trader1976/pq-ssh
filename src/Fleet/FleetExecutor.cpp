#include "FleetExecutor.h"

#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>
#include <QUuid>
#include <functional>
#include <QSharedPointer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include "../AuditLogger.h"



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

    // ---- Audit: target start ----
    AuditLogger::writeEvent("fleet.target.start", {
        {"jobId", m_job.id},
        {"profileIndex", profileIndex},
        {"profileName", p.name},
        {"group", p.group},
        {"user", p.user},
        {"host", p.host},
        {"port", r.port},
        {"actionType", (int)action.type},
        {"actionTitle", action.title},
    });

    if (m_cancelRequested.loadAcquire() != 0) {
        r.state = FleetTargetState::Canceled;
        r.error = "Canceled";

        AuditLogger::writeEvent("fleet.target.canceled", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"reason", "cancel_requested_before_connect"}
        });

        return r;
    }

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        r.state = FleetTargetState::Failed;
        r.error = "Empty user/host";

        AuditLogger::writeEvent("fleet.target.failed", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"reason", "empty_user_or_host"}
        });

        return r;
    }

    const QString cmd = buildCommand(action);
    if (cmd.trimmed().isEmpty()) {
        r.state = FleetTargetState::Failed;
        r.error = "Empty command/service";

        AuditLogger::writeEvent("fleet.target.failed", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"reason", "empty_command"}
        });

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

        AuditLogger::writeEvent("fleet.target.connect_failed", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"durationMs", (int)r.durationMs},
            {"error", err.left(400)} // cap
        });

        return r;
    }

    QString out, e;

    // Fleet policy: command timeout
    const int timeoutMs =
        (m_commandTimeoutMs > 0) ? m_commandTimeoutMs : (90 * 1000); // default 90s

    // Requires:
    // bool exec(const QString& command, QString* out, QString* err, int timeoutMs);
    const bool ok = client.exec(cmd, &out, &e, timeoutMs);

    client.disconnect();

    r.durationMs = t.elapsed();

    // Store full stdout/stderr in memory (UI uses preview columns)
    r.stdoutText = out;
    r.stderrText = e;

    // ---- Audit: after execution (this is your key point) ----
    // Do NOT log full stdout/stderr (could contain secrets). Log small previews only.
    const QString outPreview = out.trimmed().left(240);
    const QString errPreview = e.trimmed().left(240);

    AuditLogger::writeEvent("fleet.target.exec_done", {
        {"jobId", m_job.id},
        {"profileIndex", profileIndex},
        {"profileName", p.name},
        {"durationMs", (int)r.durationMs},
        {"timeoutMs", timeoutMs},
        {"ok", ok},
        {"stdoutPreview", outPreview},
        {"stderrPreview", errPreview}
    });

    if (m_cancelRequested.loadAcquire() != 0) {
        r.state = FleetTargetState::Canceled;
        r.error = "Canceled";

        AuditLogger::writeEvent("fleet.target.canceled", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"reason", "cancel_requested_after_exec"}
        });

        return r;
    }

    if (!ok) {
        const QString trimmed = e.trimmed();
        const bool looksTimeout =
            trimmed.contains("timed out", Qt::CaseInsensitive) ||
            trimmed.contains("timeout", Qt::CaseInsensitive);

        r.state = FleetTargetState::Failed;
        r.error = looksTimeout
                    ? QString("Timeout after %1 ms").arg(timeoutMs)
                    : (trimmed.isEmpty() ? "Command failed" : trimmed);

        AuditLogger::writeEvent("fleet.target.failed", {
            {"jobId", m_job.id},
            {"profileIndex", profileIndex},
            {"profileName", p.name},
            {"durationMs", (int)r.durationMs},
            {"timeoutMs", timeoutMs},
            {"reason", looksTimeout ? "timeout" : "exec_failed"},
            {"error", r.error.left(400)}
        });

        return r;
    }

    r.state = FleetTargetState::Ok;

    AuditLogger::writeEvent("fleet.target.success", {
        {"jobId", m_job.id},
        {"profileIndex", profileIndex},
        {"profileName", p.name},
        {"durationMs", (int)r.durationMs},
        {"timeoutMs", timeoutMs}
    });

    return r;
}
