#include "SshShellWorker.h"
#include <QDebug>
#include <QThread>

/*
 * SshShellWorker
 * --------------
 * Background worker responsible for running an interactive SSH shell
 * over a libssh session and streaming I/O to/from the UI.
 *
 * Design goals:
 *  - Keep ALL blocking SSH I/O off the UI thread
 *  - Provide a simple byte-stream interface (input/output)
 *  - Cleanly terminate on disconnect or remote EOF
 *
 * This worker is typically moved to its own QThread.
 */

// ===========================================================================
// Helper: open PTY-backed shell
// ===========================================================================

/*
 * openPtyShell
 * ------------
 * Creates and opens a libssh channel with:
 *  - Session channel
 *  - Pseudo-terminal (PTY)
 *  - Interactive shell
 *
 * This function performs the minimal, stable sequence that works
 * reliably across libssh versions.
 *
 * @param session  Connected libssh session
 * @param cols     Terminal column count (currently unused by libssh here)
 * @param rows     Terminal row count    (currently unused by libssh here)
 * @param term     Terminal type string
 *
 * @return         Open ssh_channel on success, nullptr on failure
 */
static ssh_channel openPtyShell(ssh_session session,
                                int cols,
                                int rows,
                                const char *term = "xterm-color")
{
    Q_UNUSED(cols);
    Q_UNUSED(rows);
    Q_UNUSED(term);

    if (!session)
        return nullptr;

    ssh_channel channel = ssh_channel_new(session);
    if (!channel)
        return nullptr;

    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return nullptr;
    }

    // Request a basic PTY.
    // (ssh_channel_request_pty_size() is avoided for compatibility)
    if (ssh_channel_request_pty(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
    }

    // Request an interactive shell on the PTY
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
    }

    return channel;
}

// ===========================================================================
// Construction
// ===========================================================================

SshShellWorker::SshShellWorker(ssh_session session,
                               int cols,
                               int rows,
                               QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_cols(cols)
    , m_rows(rows)
{
    // Note:
    // - session is owned by SshClient
    // - this worker only borrows it for the shell lifetime
}

// ===========================================================================
// Shell lifecycle
// ===========================================================================

/*
 * startShell
 * ----------
 * Entry point executed inside the worker thread.
 *
 * Responsibilities:
 *  - Open PTY-backed shell channel
 *  - Continuously read remote output (non-blocking)
 *  - Emit outputReady() for UI consumption
 *  - Detect remote EOF / close
 *  - Clean up channel resources
 *  - Emit shellClosed() with a human-readable reason
 */
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

    // Main I/O loop:
    // - Non-blocking reads
    // - Small sleep to avoid busy-spinning
    while (m_running.loadRelaxed()) {

        int n = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 0);
        if (n > 0) {
            emit outputReady(QByteArray(buf, n));
        }

        // Remote side closed the channel or sent EOF
        if (ssh_channel_is_eof(m_channel) || ssh_channel_is_closed(m_channel)) {
            qDebug() << "[SshShellWorker] channel EOF or closed";
            break;
        }

        QThread::msleep(5);
    }

    // -----------------------------------------------------------------------
    // Cleanup + exit reporting
    // -----------------------------------------------------------------------

    int exitStatus = -1;

    if (m_channel) {
        // libssh contract:
        // exit status is valid only AFTER channel is closed
        exitStatus = ssh_channel_get_exit_status(m_channel);

        ssh_channel_send_eof(m_channel);
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }

    QString reason;
    if (exitStatus < 0) {
        reason = QStringLiteral("Shell terminated");
    } else if (exitStatus == 0) {
        reason = QStringLiteral("Shell exited normally");
    } else {
        reason = QString("Shell exited with status %1").arg(exitStatus);
    }

    qDebug() << "[SshShellWorker] leaving startShell(), exitStatus =" << exitStatus;
    emit shellClosed(reason);
}

/*
 * stopShell
 * ---------
 * Requests the shell loop to exit.
 *
 * This does NOT immediately close the channel.
 * Instead:
 *  - The read loop notices m_running == false
 *  - Cleanup happens in startShell()
 *
 * Safe to call from any thread.
 */
void SshShellWorker::stopShell()
{
    qDebug() << "[SshShellWorker] stopShell() called";
    m_running.storeRelaxed(false);
}

// ===========================================================================
// Input handling
// ===========================================================================

/*
 * sendInput
 * ---------
 * Writes raw input bytes to the remote shell.
 *
 * Notes:
 *  - Called from UI thread via queued signal/slot
 *  - Performs minimal translation (CR -> CRLF)
 *  - Does nothing if the shell is not running
 */
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

    // Normalize Enter key for shells
    if (toSend == "\r") {
        toSend = "\r\n";
    }

    int rc = ssh_channel_write(m_channel, toSend.constData(), toSend.size());
    Q_UNUSED(rc);
}
