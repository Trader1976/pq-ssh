#pragma once

#include <QObject>
#include <QAtomicInteger>
#include <QByteArray>

#include <libssh/libssh.h>

/**
 * SshShellWorker
 *
 * Purpose:
 *  Runs an interactive SSH shell (PTY + shell) over an already-established
 *  libssh session, and streams data asynchronously between SSH and the UI.
 *
 * Design:
 *  - This object is meant to live in its own QThread
 *  - It does NOT create or authenticate the SSH session
 *  - It only:
 *      • opens a PTY
 *      • starts a shell
 *      • reads/writes bytes
 *
 * Typical lifecycle:
 *  1) MainWindow / controller creates ssh_session via SshClient
 *  2) SshShellWorker is constructed with that session
 *  3) startShell() is invoked in a worker thread
 *  4) outputReady() emits data → UI terminal widget
 *  5) stopShell() stops the loop and closes the channel
 *
 * Threading model:
 *  - startShell() runs a blocking read loop
 *  - Communication with UI is signal/slot based
 */
class SshShellWorker : public QObject
{
    Q_OBJECT
public:
    /**
     * Constructor
     *
     * @param session  An already-authenticated libssh session
     * @param cols     Initial terminal width (columns)
     * @param rows     Initial terminal height (rows)
     *
     * Note:
     *  Ownership of ssh_session is NOT transferred.
     *  The session must outlive this worker.
     */
    explicit SshShellWorker(ssh_session session,
                            int cols,
                            int rows,
                            QObject *parent = nullptr);

public slots:
    /**
     * startShell()
     *
     * Opens a PTY + interactive shell on the SSH session and
     * enters a read loop.
     *
     * This method:
     *  - MUST be run in a background thread
     *  - Blocks until shell exits or stopShell() is called
     *
     * Emits:
     *  - outputReady() when remote data arrives
     *  - shellClosed() when the shell ends
     */
    void startShell();

    /**
     * stopShell()
     *
     * Signals the worker loop to exit gracefully.
     * Safe to call from another thread.
     */
    void stopShell();

    /**
     * sendInput()
     *
     * Sends raw bytes to the remote shell.
     * Typically connected to a terminal widget's key input.
     *
     * @param data Raw byte stream (UTF-8, control chars, etc.)
     */
    void sendInput(const QByteArray &data);

signals:
    /**
     * outputReady
     *
     * Emitted when data is read from the SSH channel.
     * UI layer is responsible for rendering.
     */
    void outputReady(const QByteArray &data);

    /**
     * shellClosed
     *
     * Emitted when the shell terminates or the channel closes.
     *
     * @param reason Human-readable reason (normal exit, error, etc.)
     */
    void shellClosed(const QString &reason);

private:
    /**
     * Underlying SSH session (NOT owned)
     */
    ssh_session           m_session = nullptr;

    /**
     * Active shell channel (owned by this worker)
     */
    ssh_channel           m_channel = nullptr;

    /**
     * Terminal geometry at shell startup
     */
    int                   m_cols    = 80;
    int                   m_rows    = 24;

    /**
     * Atomic run flag used by startShell() loop
     *
     * - true  → keep reading
     * - false → exit loop and clean up
     */
    QAtomicInteger<bool>  m_running { false };
};
