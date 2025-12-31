#pragma once

#include <QDialog>
#include <QVector>

#include "ScheduledJob.h"
#include "SshProfile.h"
#include "SshClient.h"

class QTableWidget;
class QPushButton;


enum class JobBackend {
    SystemdUser,
    At,
    Cron,
    None
};
struct JobBackendCaps {
    bool systemd = false;      // systemctl exists
    bool userBus = false;      // /run/user/<uid>/bus exists (for systemctl --user)
    bool cron = false;         // crontab exists
    bool at = false;           // at exists
    QString details;
};
class ScheduledJobsDialog : public QDialog
{
    Q_OBJECT
public:
    ScheduledJobsDialog(const QVector<SshProfile>& profiles,
                        QVector<ScheduledJob> jobs,
                        SshClient* ssh,
                        QWidget* parent = nullptr);

    QVector<ScheduledJob> resultJobs() const { return m_jobs; }

private slots:
    void onAdd();
    void onEdit();
    void onDelete();
    void onInstall();
    void onCancelRemote();

private:
    // ---- Backend selection ----
    enum class JobBackend { None, SystemdUser, Cron, At };

    struct JobBackendCaps {
        bool systemdUser = false;
        bool cron = false;
        bool at = false;
        QString details;
    };

    JobBackendCaps probeBackendCaps(QString* err) const;
    JobBackend chooseBackend(const JobBackendCaps& caps, const ScheduledJob& job) const;

    bool installRemote(const ScheduledJob& job, QString* err);
    bool cancelRemote(const ScheduledJob& job, QString* err);

    bool installRemoteSystemdUser(const ScheduledJob& job, QString* err);
    bool installRemoteCron(const ScheduledJob& job, QString* err);
    bool installRemoteAt(const ScheduledJob& job, QString* err);

    bool cancelRemoteSystemdUser(const ScheduledJob& job, QString* err);
    bool cancelRemoteCron(const ScheduledJob& job, QString* err);
    bool cancelRemoteAt(const ScheduledJob& job, QString* err);



    QString remoteHomeDir(QString* err) const;
    QString shQuote(const QString& s) const;


    // existing helpers
    void buildUi();
    void rebuildTable();
    int  currentRow() const;

    QString unitBase(const ScheduledJob& job) const;


private:
    const QVector<SshProfile>& m_profiles;
    QVector<ScheduledJob> m_jobs;
    SshClient* m_ssh = nullptr;

    QTableWidget* m_table = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_editBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QPushButton* m_installBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
};
