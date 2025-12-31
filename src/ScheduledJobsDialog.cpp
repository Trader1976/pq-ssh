#include "ScheduledJobsDialog.h"
#include "ScheduledJobStore.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QMessageBox>
#include <QInputDialog>
#include <QAbstractItemView>
#include <QTableWidgetItem>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>

// systemd OnCalendar accepts "YYYY-MM-DD HH:MM:SS"
static QString toOnCalendarOneShot(const QDateTime& local)
{
    if (!local.isValid()) return {};
    return local.toString("yyyy-MM-dd HH:mm:ss");
}

static QString kindToText(const ScheduledJob& j)
{
    return (j.kind == ScheduledJob::Kind::OneShot) ? QObject::tr("One-shot")
                                                   : QObject::tr("Recurring");
}

static bool remoteHasCmd(SshClient* ssh, const QString& cmd, QString* detailsOut = nullptr)
{
    if (!ssh) return false;
    QString out, err;

    // POSIX and fast
    const QString q = QString("/bin/sh -lc 'command -v %1 >/dev/null 2>&1'").arg(cmd);
    const bool ok = ssh->exec(q, &out, &err, 8000);

    if (detailsOut) {
        *detailsOut += QString("%1=%2\n").arg(cmd, ok ? "yes" : "no");
        if (!err.trimmed().isEmpty())
            *detailsOut += QString("  %1 err: %2\n").arg(cmd, err.trimmed());
    }
    return ok;
}




ScheduledJobsDialog::JobBackend ScheduledJobsDialog::chooseBackend(const JobBackendCaps& caps,
                                                                   const ScheduledJob& job) const
{
    // Prefer systemd user timers when actually usable (user bus present)
    if (caps.systemdUser)
        return JobBackend::SystemdUser;

    // Recurring: cron is the best fallback
    if (job.kind == ScheduledJob::Kind::Recurring)
        return caps.cron ? JobBackend::Cron : JobBackend::None;

    // One-shot: prefer at, otherwise cron (we support one-shot-via-cron with self-removal)
    if (caps.at)   return JobBackend::At;
    if (caps.cron) return JobBackend::Cron;

    return JobBackend::None;
}

bool ScheduledJobsDialog::installRemoteSystemdUser(const ScheduledJob& job, QString* err)
{
    // This is basically your existing systemd-user install logic.
    // Keep using absolute $HOME to avoid "~" SFTP issues.
    if (!m_ssh) { if (err) *err = tr("SSH client missing."); return false; }
    const QString base = unitBase(job);

    QString eHome;
    const QString home = remoteHomeDir(&eHome);
    if (home.isEmpty()) { if (err) *err = eHome; return false; }

    const QString userDir = home + "/.config/systemd/user";

    QString onCal;
    if (job.kind == ScheduledJob::Kind::OneShot) {
        onCal = toOnCalendarOneShot(job.runAtLocal);
        if (onCal.isEmpty()) { if (err) *err = tr("Run time not set."); return false; }
    } else {
        onCal = job.onCalendar.trimmed();
        if (onCal.isEmpty()) { if (err) *err = tr("OnCalendar not set."); return false; }
    }

    const QString cmdQuoted = shQuote(job.command);

    const QString serviceText =
        QString(
            "[Unit]\n"
            "Description=PQ-SSH Scheduled Job (%1)\n"
            "\n"
            "[Service]\n"
            "Type=oneshot\n"
            "ExecStart=/bin/sh -lc %2\n"
        ).arg(base, cmdQuoted);

    const QString timerText =
        QString(
            "[Unit]\n"
            "Description=PQ-SSH Timer (%1)\n"
            "\n"
            "[Timer]\n"
            "OnCalendar=%2\n"
            "Persistent=true\n"
            "\n"
            "[Install]\n"
            "WantedBy=timers.target\n"
        ).arg(base, onCal);

    QString e;
    if (!m_ssh->exec(QString("mkdir -p %1").arg(shQuote(userDir)), nullptr, &e, 8000)) {
        if (err) *err = e;
        return false;
    }

    const QString svcPath = userDir + "/" + base + ".service";
    const QString tmrPath = userDir + "/" + base + ".timer";

    if (!m_ssh->writeRemoteTextFileAtomic(svcPath, serviceText, 0644, &e)) { if (err) *err = e; return false; }
    if (!m_ssh->writeRemoteTextFileAtomic(tmrPath, timerText, 0644, &e)) { if (err) *err = e; return false; }

    // Determine uid
    QString uidOut;
    if (!m_ssh->exec("/bin/sh -lc 'id -u'", &uidOut, &e, 8000)) {
        if (err) *err = e.isEmpty() ? tr("Could not determine remote uid.") : e;
        return false;
    }
    bool okUid = false;
    const int uid = uidOut.trimmed().toInt(&okUid);
    if (!okUid || uid <= 0) {
        if (err) *err = tr("Invalid remote uid output: %1").arg(uidOut.trimmed());
        return false;
    }

    // Use explicit env for user bus
    const QString sysctlPrefix =
        QString("/bin/sh -lc 'XDG_RUNTIME_DIR=/run/user/%1 "
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus "
                "systemctl --user ")
            .arg(uid);

    if (!m_ssh->exec(sysctlPrefix + "daemon-reload'", nullptr, &e, 8000)) { if (err) *err = e; return false; }
    if (!m_ssh->exec(sysctlPrefix + QString("enable --now %1.timer'").arg(base), nullptr, &e, 8000)) { if (err) *err = e; return false; }

    if (err) err->clear();
    return true;
}

bool ScheduledJobsDialog::installRemoteCron(const ScheduledJob& job, QString* err)
{
    if (!m_ssh) { if (err) *err = tr("SSH client missing."); return false; }

    const QString marker = QString("PQSSH:%1").arg(job.id.trimmed());
    const QString markerComment = QString("# %1").arg(marker);

    QString cronSpec;
    QString payloadCmd = job.command;

    if (job.kind == ScheduledJob::Kind::Recurring) {
        cronSpec = job.onCalendar.trimmed();

        const int fields = cronSpec.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
        if (fields < 5) {
            if (err) *err = tr("Cron backend requires a 5-field cron schedule, e.g. \"0 2 * * 1-5\".");
            return false;
        }
    } else {
        if (!job.runAtLocal.isValid()) { if (err) *err = tr("Run time not set."); return false; }
        const QDateTime t = job.runAtLocal;
        cronSpec = QString("%1 %2 %3 %4 *")
                       .arg(t.time().minute())
                       .arg(t.time().hour())
                       .arg(t.date().day())
                       .arg(t.date().month());

        // Self-delete after running: remove lines containing marker (fixed-string)
        const QString selfRemove =
            QString("crontab -l 2>/dev/null | grep -v -F %1 | crontab -")
                .arg(shQuote(markerComment));
        payloadCmd = QString("%1; %2").arg(job.command, selfRemove);
    }

    // Install cron line: schedule + /bin/sh -lc '<cmd>' + marker comment
    const QString line = QString("%1 /bin/sh -lc %2 %3")
                             .arg(cronSpec,
                                  shQuote(payloadCmd),
                                  markerComment);

    QString e;

    // Rebuild crontab: remove old marker lines, append new line.
    // Use grep -v -F '# PQSSH:...' (fixed string) for exact safety.
    const QString remote =
        QString("/bin/sh -lc '"
                "tmp=$(mktemp); "
                "crontab -l 2>/dev/null | grep -v -F %1 > \"$tmp\" || true; "
                "printf \"%%s\\n\" %2 >> \"$tmp\"; "
                "crontab \"$tmp\"; "
                "rm -f \"$tmp\""
                "'")
            .arg(shQuote(markerComment),
                 shQuote(line));

    if (!m_ssh->exec(remote, nullptr, &e, 12000)) {
        if (err) *err = e.isEmpty() ? tr("Failed to install cron job.") : e;
        return false;
    }

    if (err) err->clear();
    return true;
}





ScheduledJobsDialog::JobBackendCaps ScheduledJobsDialog::probeBackendCaps(QString* err) const
{
    JobBackendCaps caps;

    if (!m_ssh) {
        if (err) *err = tr("SSH client missing.");
        return caps;
    }

    // Check scheduler commands
    const bool hasSystemctl = remoteHasCmd(m_ssh, "systemctl", &caps.details);
    caps.cron = remoteHasCmd(m_ssh, "crontab",  &caps.details);
    caps.at   = remoteHasCmd(m_ssh, "at",       &caps.details);

    // systemd user timers require /run/user/<uid>/bus to exist for this login
    caps.systemdUser = false;
    if (hasSystemctl) {
        QString uidOut, e;
        if (m_ssh->exec("/bin/sh -lc 'id -u'", &uidOut, &e, 8000)) {
            bool okUid = false;
            const int uid = uidOut.trimmed().toInt(&okUid);

            if (okUid && uid > 0) {
                const QString busCheck = QString("/bin/sh -lc 'test -S /run/user/%1/bus'").arg(uid);
                caps.systemdUser = m_ssh->exec(busCheck, nullptr, nullptr, 8000);
                caps.details += QString("systemdUser(bus /run/user/%1/bus)=%2\n")
                                    .arg(uid)
                                    .arg(caps.systemdUser ? "yes" : "no");
            } else {
                caps.details += QString("id -u invalid: '%1'\n").arg(uidOut.trimmed());
            }
        } else {
            caps.details += QString("id -u failed: %1\n").arg(e.trimmed());
        }
    }

    if (err) err->clear();
    return caps;
}



static int findProfileIndexById(const QVector<SshProfile>& profiles, const QString& id)
{
    const QString want = id.trimmed();
    if (want.isEmpty()) return -1;

    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles[i].id.trimmed() == want)
            return i;
    }
    return -1;
}
QString ScheduledJobsDialog::remoteHomeDir(QString* err) const
{
    if (!m_ssh) {
        if (err) *err = tr("SSH client missing.");
        return {};
    }

    QString out, e;

    // 1) Primary: $HOME (fast, standard)
    if (m_ssh->exec("/bin/sh -lc 'printf \"%s\" \"$HOME\"'", &out, &e, 8000)) {
        out = out.trimmed();
        if (!out.isEmpty())
            return out;
    }

    // 2) Fallback: getent passwd
    out.clear();
    e.clear();
    if (m_ssh->exec("/bin/sh -lc 'getent passwd \"$(id -un)\" | cut -d: -f6'", &out, &e, 8000)) {
        out = out.trimmed();
        if (!out.isEmpty())
            return out;
    }

    if (err) {
        *err = tr("Could not determine remote home directory.\n"
                  "Neither $HOME nor getent passwd returned a usable path.");
    }
    return {};
}

static QString profileDisplayNameByIndex(const QVector<SshProfile>& profiles, int idx)
{
    if (idx < 0 || idx >= profiles.size()) return QObject::tr("(missing)");
    const auto& p = profiles[idx];

    const QString nm = p.name.trimmed();
    if (!nm.isEmpty()) return nm;

    return QString("%1@%2").arg(p.user, p.host);
}

static QString profileNameById(const QVector<SshProfile>& profiles, const QString& id)
{
    const int idx = findProfileIndexById(profiles, id);
    return profileDisplayNameByIndex(profiles, idx);
}



ScheduledJobsDialog::ScheduledJobsDialog(const QVector<SshProfile>& profiles,
                                         QVector<ScheduledJob> jobs,
                                         SshClient* ssh,
                                         QWidget* parent)
    : QDialog(parent),
      m_profiles(profiles),
      m_jobs(std::move(jobs)),
      m_ssh(ssh)
{
    setWindowTitle(tr("Scheduled jobs"));
    resize(860, 420);
    buildUi();
    rebuildTable();
}

void ScheduledJobsDialog::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({
        tr("Name"),
        tr("Profile"),
        tr("Type"),
        tr("When"),
        tr("Command"),
        tr("Installed"),
        tr("Enabled")
    });

    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);

    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    outer->addWidget(m_table, 1);

    auto* row = new QWidget(this);
    auto* rowL = new QHBoxLayout(row);
    rowL->setContentsMargins(0, 0, 0, 0);
    rowL->setSpacing(6);

    m_addBtn = new QPushButton(tr("Add"), row);
    m_editBtn = new QPushButton(tr("Edit"), row);
    m_delBtn = new QPushButton(tr("Delete"), row);

    m_installBtn = new QPushButton(tr("Install on server"), row);
    m_cancelBtn  = new QPushButton(tr("Cancel on server"), row);

    rowL->addWidget(m_addBtn);
    rowL->addWidget(m_editBtn);
    rowL->addWidget(m_delBtn);
    rowL->addSpacing(12);
    rowL->addWidget(m_installBtn);
    rowL->addWidget(m_cancelBtn);
    rowL->addStretch(1);

    auto* closeBtn = new QPushButton(tr("Close"), row);
    rowL->addWidget(closeBtn);

    outer->addWidget(row, 0);

    connect(m_addBtn, &QPushButton::clicked, this, &ScheduledJobsDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &ScheduledJobsDialog::onEdit);
    connect(m_delBtn, &QPushButton::clicked, this, &ScheduledJobsDialog::onDelete);
    connect(m_installBtn, &QPushButton::clicked, this, &ScheduledJobsDialog::onInstall);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ScheduledJobsDialog::onCancelRemote);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

int ScheduledJobsDialog::currentRow() const
{
    if (!m_table) return -1;
    const auto sel = m_table->selectionModel();
    if (!sel || !sel->hasSelection()) return -1;
    return sel->selectedRows().first().row();
}

void ScheduledJobsDialog::rebuildTable()
{
    if (!m_table) return;

    m_table->setRowCount(m_jobs.size());

    for (int i = 0; i < m_jobs.size(); ++i) {
        const auto& j = m_jobs[i];

        const QString profName = profileNameById(m_profiles, j.profileId);

        QString when;
        if (j.kind == ScheduledJob::Kind::OneShot) {
            when = j.runAtLocal.isValid()
                       ? j.runAtLocal.toString("yyyy-MM-dd HH:mm")
                       : tr("(not set)");
        } else {
            when = j.onCalendar.trimmed().isEmpty() ? tr("(not set)") : j.onCalendar.trimmed();
        }

        auto set = [&](int col, const QString& text) {
            m_table->setItem(i, col, new QTableWidgetItem(text));
        };

        set(0, j.name.trimmed().isEmpty() ? tr("(unnamed)") : j.name.trimmed());
        set(1, profName);
        set(2, kindToText(j));
        set(3, when);

        QString cmd = j.command.trimmed();
        if (cmd.size() > 120) cmd = cmd.left(120) + "…";
        set(4, cmd);

        set(5, j.installed ? tr("Yes") : tr("No"));
        set(6, j.enabled   ? tr("Yes") : tr("No"));
    }

    if (!m_jobs.isEmpty() && currentRow() < 0)
        m_table->selectRow(0);
}

QString ScheduledJobsDialog::unitBase(const ScheduledJob& job) const
{
    return QString("pqssh-job-%1").arg(job.id.trimmed());
}

QString ScheduledJobsDialog::shQuote(const QString& s) const
{
    QString out = s;
    out.replace("'", "'\"'\"'");
    return "'" + out + "'";
}
bool ScheduledJobsDialog::installRemoteAt(const ScheduledJob& job, QString* err)
{
    if (!m_ssh) { if (err) *err = tr("SSH client missing."); return false; }

    // "at" is one-shot only
    if (job.kind != ScheduledJob::Kind::OneShot) {
        if (err) *err = tr("The 'at' backend supports one-shot jobs only.");
        return false;
    }
    if (!job.runAtLocal.isValid()) {
        if (err) *err = tr("Run time not set.");
        return false;
    }

    QString eHome;
    const QString home = remoteHomeDir(&eHome);
    if (home.isEmpty()) {
        if (err) *err = eHome.isEmpty() ? tr("Could not resolve remote home directory.") : eHome;
        return false;
    }

    // Store a script on remote and enqueue it with at -t
    const QString dir = home + "/.config/pq-ssh/jobs";
    const QString scriptPath = dir + "/" + job.id.trimmed() + ".sh";

    const QString script =
        QString("#!/bin/sh\n"
                "set -e\n"
                "/bin/sh -lc %1\n").arg(shQuote(job.command));

    QString e;
    if (!m_ssh->exec(QString("/bin/sh -lc 'mkdir -p %1'").arg(shQuote(dir)), nullptr, &e, 8000)) {
        if (err) *err = e;
        return false;
    }

    if (!m_ssh->writeRemoteTextFileAtomic(scriptPath, script, 0755, &e)) {
        if (err) *err = e;
        return false;
    }

    // at -t format: [[CC]YY]MMDDhhmm
    const QString atTime = job.runAtLocal.toString("yyyyMMddHHmm");

    // Enqueue
    const QString enqueue =
        QString("/bin/sh -lc 'at -t %1 -f %2'")
            .arg(atTime, shQuote(scriptPath));

    if (!m_ssh->exec(enqueue, nullptr, &e, 12000)) {
        if (err) *err = e.isEmpty() ? tr("Failed to enqueue at job.") : e;
        return false;
    }

    if (err) err->clear();
    return true;
}

bool ScheduledJobsDialog::installRemote(const ScheduledJob& job, QString* err)
{
    if (!m_ssh) { if (err) *err = tr("SSH client missing."); return false; }

    // Resolve profile by stable id (NOT by index)
    const int pIdx = findProfileIndexById(m_profiles, job.profileId);
    if (pIdx < 0 || pIdx >= m_profiles.size()) {
        if (err) *err = tr("Profile not found (missing).");
        return false;
    }
    const auto& p = m_profiles[pIdx];

    // Connect first (probeBackendCaps needs exec)
    QString e;
    if (!m_ssh->connectProfile(p, &e)) {
        if (err) *err = e;
        return false;
    }

    // Probe capabilities now that we are connected
    const JobBackendCaps caps = probeBackendCaps(nullptr);
    qInfo().noquote() << "Scheduled job backend caps:\n" << caps.details;

    const JobBackend backend = chooseBackend(caps, job);
    if (backend == JobBackend::None) {
        if (err) *err = tr("No supported scheduler found on the remote.\n\nDetails:\n%1").arg(caps.details);
        return false;
    }

    switch (backend) {
    case JobBackend::SystemdUser: return installRemoteSystemdUser(job, err);
    case JobBackend::Cron:        return installRemoteCron(job, err);
    case JobBackend::At:          return installRemoteAt(job, err);
    default: break;
    }

    if (err) *err = tr("Unsupported scheduler backend.");
    return false;
}




bool ScheduledJobsDialog::cancelRemote(const ScheduledJob& job, QString* err)
{
    if (!m_ssh) { if (err) *err = tr("SSH client missing."); return false; }

    const int pIdx = findProfileIndexById(m_profiles, job.profileId);
    if (pIdx < 0 || pIdx >= m_profiles.size()) {
        if (err) *err = tr("Profile not found (missing).");
        return false;
    }
    const auto& p = m_profiles[pIdx];

    QString e;
    if (!m_ssh->connectProfile(p, &e)) {
        if (err) *err = e;
        return false;
    }

    const QString base = unitBase(job);

    // Resolve $HOME on remote (avoid "~" with SFTP paths)
    QString eHome;
    const QString home = remoteHomeDir(&eHome);
    if (home.isEmpty()) { if (err) *err = eHome.isEmpty() ? tr("Could not resolve remote home directory.") : eHome; return false; }

    const QString userDir = home + "/.config/systemd/user";
    const QString svcPath = userDir + "/" + base + ".service";
    const QString tmrPath = userDir + "/" + base + ".timer";

    // Determine uid (for user bus path)
    const QString uidCmd = "/bin/sh -lc 'id -u'";
    QString uidOut;
    if (!m_ssh->exec(uidCmd, &uidOut, &e, 8000)) {
        if (err) *err = e.isEmpty() ? tr("Could not determine remote uid.") : e;
        return false;
    }

    bool okUid = false;
    const int uid = uidOut.trimmed().toInt(&okUid);
    if (!okUid || uid <= 0) {
        if (err) *err = tr("Invalid remote uid output: %1").arg(uidOut.trimmed());
        return false;
    }

    // Probe user bus
    const QString busCheck =
        QString("/bin/sh -lc 'test -S /run/user/%1/bus'").arg(uid);

    if (!m_ssh->exec(busCheck, nullptr, &e, 8000)) {
        if (err) {
            *err =
                tr("This server does not have a user systemd/DBus session available for this SSH login.\n\n"
                   "systemctl --user needs /run/user/%1/bus, but it was not found.\n\n"
                   "Fix options:\n"
                   "  1) (Recommended) Enable lingering once:\n"
                   "     sudo loginctl enable-linger %2\n"
                   "  2) Log in with a full desktop/session that provides a user bus.\n"
                   "  3) Use a cron/at backend instead of systemd user timers.")
                    .arg(uid)
                    .arg(p.user.trimmed().isEmpty() ? QStringLiteral("$USER") : p.user.trimmed());
        }
        return false;
    }

    // systemctl --user env wrapper (works for non-interactive SSH exec if bus exists)
    const QString sysctlUser =
        QString("/bin/sh -lc 'XDG_RUNTIME_DIR=/run/user/%1 "
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus "
                "systemctl --user ")
            .arg(uid);

    // Best-effort stop/disable + remove files
    m_ssh->exec(sysctlUser + QString("disable --now %1.timer'").arg(base), nullptr, nullptr, 8000);
    m_ssh->exec(QString("/bin/sh -lc 'rm -f %1 %2'")
                    .arg(shQuote(svcPath), shQuote(tmrPath)),
                nullptr, nullptr, 8000);

    // Always try reload; this is the one we treat as "real" failure
    if (!m_ssh->exec(sysctlUser + "daemon-reload'", nullptr, &e, 8000)) {
        if (err) *err = e;
        return false;
    }

    return true;
}

// ---------- UI actions ----------

void ScheduledJobsDialog::onAdd()
{
    if (m_profiles.isEmpty()) {
        QMessageBox::warning(this, tr("No profiles"), tr("No SSH profiles exist. Create a profile first."));
        return;
    }

    ScheduledJob j;
    j.id = newJobId();
    j.name = tr("New job");
    j.command = "echo hello";
    j.enabled = true;
    j.kind = ScheduledJob::Kind::OneShot;
    j.runAtLocal = QDateTime::currentDateTime().addSecs(300);

    bool ok = false;

    // Profile select FIRST (so we always store profileId)
    QStringList profs;
    for (const auto& p : m_profiles)
        profs << profileDisplayNameByIndex(m_profiles, (&p - m_profiles.data())); // safe index derivation

    // (Simpler + safer than pointer math)
    profs.clear();
    for (int i = 0; i < m_profiles.size(); ++i)
        profs << profileDisplayNameByIndex(m_profiles, i);

    const QString picked = QInputDialog::getItem(this, tr("Profile"), tr("Run on server:"), profs, 0, false, &ok);
    if (!ok) return;

    const int pIdx = profs.indexOf(picked);
    if (pIdx < 0 || pIdx >= m_profiles.size()) {
        QMessageBox::warning(this, tr("Profile"), tr("Invalid profile selection."));
        return;
    }
    j.profileId = m_profiles[pIdx].id;

    const QString name = QInputDialog::getText(this, tr("Job name"), tr("Name:"), QLineEdit::Normal, j.name, &ok);
    if (!ok) return;
    j.name = name.trimmed();

    const QString cmd = QInputDialog::getMultiLineText(this, tr("Command"), tr("Command to run on server:"), j.command, &ok);
    if (!ok) return;
    j.command = cmd;

    // Choose kind (DO NOT compare translated prefixes)
    const QStringList kinds = { tr("One-shot (run once)"), tr("Recurring (OnCalendar)") };
    const QString k = QInputDialog::getItem(this, tr("Schedule type"), tr("Type:"), kinds, 0, false, &ok);
    if (!ok) return;

    if (k == kinds[1]) {
        j.kind = ScheduledJob::Kind::Recurring;
        const QString oc = QInputDialog::getText(
                               this,
                               tr("OnCalendar"),
                               tr("systemd OnCalendar (e.g. daily, Mon..Fri 02:00):"),
                               QLineEdit::Normal,
                               "daily",
                               &ok
                           ).trimmed();
        if (!ok) return;
        j.onCalendar = oc;
    } else {
        j.kind = ScheduledJob::Kind::OneShot;
        const QString when = QInputDialog::getText(
                                 this,
                                 tr("Run at"),
                                 tr("Local time (YYYY-MM-DD HH:MM):"),
                                 QLineEdit::Normal,
                                 j.runAtLocal.toString("yyyy-MM-dd HH:mm"),
                                 &ok
                             );
        if (!ok) return;

        const QDateTime dt = QDateTime::fromString(when.trimmed(), "yyyy-MM-dd HH:mm");
        if (!dt.isValid()) {
            QMessageBox::warning(this, tr("Run at"), tr("Invalid time format. Use YYYY-MM-DD HH:MM."));
            return;
        }
        j.runAtLocal = dt;
    }

    m_jobs.push_back(j);
    rebuildTable();
}

void ScheduledJobsDialog::onEdit()
{
    const int row = currentRow();
    if (row < 0 || row >= m_jobs.size()) return;

    ScheduledJob& j = m_jobs[row];

    bool ok = false;

    const QString name = QInputDialog::getText(this, tr("Job name"), tr("Name:"), QLineEdit::Normal, j.name, &ok);
    if (!ok) return;
    j.name = name.trimmed();

    const QString cmd = QInputDialog::getMultiLineText(this, tr("Command"), tr("Command:"), j.command, &ok);
    if (!ok) return;
    j.command = cmd;

    if (j.kind == ScheduledJob::Kind::Recurring) {
        const QString oc = QInputDialog::getText(this, tr("OnCalendar"), tr("OnCalendar:"), QLineEdit::Normal, j.onCalendar, &ok);
        if (!ok) return;
        j.onCalendar = oc.trimmed();
    } else {
        const QString when = QInputDialog::getText(this, tr("Run at"), tr("Local time (YYYY-MM-DD HH:MM):"),
                                                   QLineEdit::Normal, j.runAtLocal.toString("yyyy-MM-dd HH:mm"), &ok);
        if (!ok) return;

        const QDateTime dt = QDateTime::fromString(when.trimmed(), "yyyy-MM-dd HH:mm");
        if (!dt.isValid()) {
            QMessageBox::warning(this, tr("Run at"), tr("Invalid time format. Use YYYY-MM-DD HH:MM."));
            return;
        }
        j.runAtLocal = dt;
    }

    // Allow changing profile during edit (still store profileId)
    if (!m_profiles.isEmpty()) {
        QStringList profs;
        for (int i = 0; i < m_profiles.size(); ++i)
            profs << profileDisplayNameByIndex(m_profiles, i);

        const int curIdx = findProfileIndexById(m_profiles, j.profileId);
        const int startIdx = (curIdx >= 0) ? curIdx : 0;

        const QString picked = QInputDialog::getItem(this, tr("Profile"), tr("Run on server:"), profs, startIdx, false, &ok);
        if (ok) {
            const int idx = profs.indexOf(picked);
            if (idx >= 0 && idx < m_profiles.size())
                j.profileId = m_profiles[idx].id;
        }
    }

    rebuildTable();
}

void ScheduledJobsDialog::onDelete()
{
    const int row = currentRow();
    if (row < 0 || row >= m_jobs.size()) return;

    if (QMessageBox::question(this, tr("Delete job"),
                              tr("Delete this scheduled job from PQ-SSH?\n\n"
                                 "This does NOT cancel it on the server.\n"
                                 "Use 'Cancel on server' first if needed.")) != QMessageBox::Yes)
        return;

    m_jobs.removeAt(row);
    rebuildTable();
}

void ScheduledJobsDialog::onInstall()
{
    const int row = currentRow();
    if (row < 0 || row >= m_jobs.size()) return;

    ScheduledJob& j = m_jobs[row];

    QString err;
    if (!installRemote(j, &err)) {
        j.installed = false;
        j.lastInstallError = err;
        QMessageBox::warning(this, tr("Install failed"), err);
        rebuildTable();
        return;
    }

    j.installed = true;
    j.lastInstallError.clear();
    QMessageBox::information(this, tr("Installed"), tr("Job installed on server."));
    rebuildTable();
}

void ScheduledJobsDialog::onCancelRemote()
{
    const int row = currentRow();
    if (row < 0 || row >= m_jobs.size()) return;

    ScheduledJob& j = m_jobs[row];

    QString err;
    if (!cancelRemote(j, &err)) {
        QMessageBox::warning(this, tr("Cancel failed"), err);
        return;
    }

    j.installed = false;
    QMessageBox::information(this, tr("Cancelled"), tr("Job cancelled on server."));
    rebuildTable();
}
