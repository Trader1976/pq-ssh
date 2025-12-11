#pragma once

#include <QObject>
#include <QAtomicInteger>
#include <QByteArray>

#include <libssh/libssh.h>

class SshShellWorker : public QObject
{
    Q_OBJECT
public:
    explicit SshShellWorker(ssh_session session,
                            int cols,
                            int rows,
                            QObject *parent = nullptr);

public slots:
    void startShell();
    void stopShell();
    void sendInput(const QByteArray &data);

signals:
    void outputReady(const QByteArray &data);
    void shellClosed(const QString &reason);

private:
    ssh_session           m_session = nullptr;
    ssh_channel           m_channel = nullptr;
    int                   m_cols    = 80;
    int                   m_rows    = 24;
    QAtomicInteger<bool>  m_running { false };
};
