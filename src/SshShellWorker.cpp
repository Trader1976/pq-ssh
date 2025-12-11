#include "SshShellWorker.h"
#include <QDebug>
#include <QThread>

// Simple helper to open a PTY + shell on the given session
static ssh_channel openPtyShell(ssh_session session,
                                int cols,
                                int rows,
                                const char *term = "xterm-color")
{
    if (!session)
        return nullptr;

    ssh_channel channel = ssh_channel_new(session);
    if (!channel)
        return nullptr;

    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return nullptr;
    }

    // Request a basic PTY. This variant is stable in libssh.
    if (ssh_channel_request_pty(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
    }

    if (ssh_channel_request_shell(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
    }

    return channel;
}

SshShellWorker::SshShellWorker(ssh_session session,
                               int cols,
                               int rows,
                               QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_cols(cols)
    , m_rows(rows)
{
}

void SshShellWorker::startShell()
{
    qDebug() << "[SshShellWorker] startShell() entered, session =" << m_session;

    m_channel = openPtyShell(m_session, m_cols, m_rows);
    if (!m_channel) {
        qDebug() << "[SshShellWorker] openPtyShell FAILED";
        emit shellClosed(QStringLiteral("Failed to open PTY shell"));
        return;
    }

    qDebug() << "[SshShellWorker] openPtyShell OK, channel =" << m_channel;

    m_running.storeRelaxed(true);

    char buf[4096];

    while (m_running.loadRelaxed()) {
        int n = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 0);
        if (n > 0) {
            qDebug() << "[SshShellWorker] read" << n << "bytes from channel";
            emit outputReady(QByteArray(buf, n));
        }

        if (ssh_channel_is_eof(m_channel) || ssh_channel_is_closed(m_channel)) {
            qDebug() << "[SshShellWorker] channel EOF or closed";
            break;
        }

        QThread::msleep(5);
    }

    if (m_channel) {
        ssh_channel_send_eof(m_channel);
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }

    qDebug() << "[SshShellWorker] leaving startShell(), emitting shellClosed";
    emit shellClosed(QStringLiteral("Shell terminated"));
}



void SshShellWorker::stopShell()
{
    qDebug() << "[SshShellWorker] stopShell() called";
    m_running.storeRelaxed(false);
}




void SshShellWorker::sendInput(const QByteArray &data)
{
    if (!m_channel || !m_running.loadRelaxed()) {
        qDebug() << "[SshShellWorker] sendInput: channel invalid or not running. m_channel ="
                 << m_channel << " running =" << m_running.loadRelaxed();
        return;
    }
    if (data.isEmpty())
        return;

    QByteArray toSend = data;

    // Turn bare CR into CRLF for shells
    if (toSend == "\r") {
        toSend = "\r\n";
    }

    qDebug() << "[SshShellWorker] sendInput: writing" << toSend.size()
             << "bytes to channel. First byte =" << int((unsigned char)toSend[0]);

    int rc = ssh_channel_write(m_channel, toSend.constData(), toSend.size());
    qDebug() << "[SshShellWorker] ssh_channel_write rc =" << rc;
}
