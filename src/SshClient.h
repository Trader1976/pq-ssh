    // SshClient.h
    //
    // Purpose:
    //   Lightweight wrapper around libssh providing:
    //     - Connection/authentication for SFTP + remote exec helpers
    //     - Simple file upload/download via SFTP
    //     - Remote directory listing/stat (for file manager UI)
    //     - Idempotent authorized_keys installation (with backup)
    //     - Optional dev helper to test unlocking encrypted Dilithium keys
    //
    // Design boundary:
    //   Interactive terminal sessions are handled by spawning OpenSSH
    //   (qtermwidget + "ssh ...") elsewhere. This class is primarily used
    //   when the app needs *programmatic* operations (SFTP/exec).

    #pragma once

    #include <QObject>
    #include <QString>
    #include <QByteArray>
    #include <QVector>
    #include <functional>

    #include "SshProfile.h"
    #include <atomic>

    // Forward-declare libssh session type to avoid pulling libssh headers into the header.
    // (Keeps compile times down and avoids header coupling.)
    struct ssh_session_struct;
    using ssh_session = ssh_session_struct*;

    class SshClient : public QObject
    {
        Q_OBJECT
    public:
        explicit SshClient(QObject *parent = nullptr);
        ~SshClient() override;

        // UI-injected callback used when libssh needs a passphrase for an encrypted key.
        // - keyFile: path (or hint) for which key is being unlocked (may be empty)
        // - ok: set false if user cancelled
        // Returns passphrase text (never log it).
        using PassphraseProvider = std::function<QString(const QString& keyFile, bool *ok)>;

        // Optional progress callback for streaming transfers (done/total bytes).
        using ProgressCb = std::function<void(quint64 done, quint64 total)>;

        // Remote file listing entry (minimal metadata for a file manager view).
        struct RemoteEntry
        {
            QString name;       // basename
            QString fullPath;   // absolute/combined path (as returned/constructed)
            bool    isDir = false;

            quint64 size  = 0;  // bytes
            quint32 perms = 0;  // st_mode (permissions + type bits)
            qint64  mtime = 0;  // seconds since epoch
        };

        // Register passphrase provider (typically set by MainWindow).
        void setPassphraseProvider(PassphraseProvider cb) { m_passphraseProvider = std::move(cb); }

        // Connect using a profile (preferred modern path).
        // Supports "auto"/"openssh" key_type today.
        // On success, m_session becomes valid and SFTP/exec helpers can be used.
        bool connectProfile(const SshProfile& profile, QString* err = nullptr);

        // Backwards-compatible helper: connect using "user@host" string.
        // Builds a temporary profile (port 22, key_type auto) and calls connectProfile().
        bool connectPublicKey(const QString& target, QString* err = nullptr);

        // DEV helper: prompt passphrase + attempt to decrypt an encrypted Dilithium key blob.
        // This only validates "unlock works"; it must not expose plaintext.
        bool testUnlockDilithiumKey(const QString& encKeyPath, QString* err = nullptr);

        // Close/free current libssh session (safe to call multiple times).
        void disconnect();

        // True if a libssh session is active.
        bool isConnected() const;

        // Run remote `pwd` and return its trimmed output.
        // Used by drag-drop upload to decide where to upload by default.
        QString remotePwd(QString* err = nullptr) const;

        // ------------------------------------------------------------
        // SFTP: file manager primitives
        // ------------------------------------------------------------

        // List a remote directory (non-recursive).
        // remotePath may be ".", "~", "/path", etc. (server-dependent).
        bool listRemoteDir(const QString& remotePath,
                           QVector<RemoteEntry>* outItems,
                           QString* err = nullptr);

        // Stat a remote path (file or directory).
        bool statRemotePath(const QString& remotePath,
                            RemoteEntry* outInfo,
                            QString* err = nullptr);

        // Streaming upload local file -> remote file (create/truncate).
        // Progress callback is optional.
        bool uploadFile(const QString& localPath,
                        const QString& remotePath,
                        QString* err = nullptr,
                        ProgressCb progress = nullptr);

        // Streaming download remote file -> local file (creates local dirs if needed).
        // Progress callback is optional.
    bool downloadFile(const QString& remotePath,
                      const QString& localPath,
                      QString* err = nullptr,
                      ProgressCb progress = nullptr);

        // ------------------------------------------------------------
        // Existing helpers (kept for compatibility / small-file use)
        // ------------------------------------------------------------

        // Upload raw bytes to a remote file via SFTP (create/truncate).
        // NOTE: not ideal for huge data; prefer uploadFile() for real files.
        bool uploadBytes(const QString& remotePath, const QByteArray& data, QString* err = nullptr);

        // Download remote file via SFTP to a local path (creates local dirs if needed).
        // NOTE: current implementation reads whole file into memory; prefer downloadFile().
        bool downloadToFile(const QString& remotePath, const QString& localPath, QString* err = nullptr);

        // Execute a remote command, capture stdout/stderr, and fail if exit status != 0.
        // SECURITY NOTE: callers must quote/sanitize user-controlled inputs.
        bool exec(const QString& command, QString* out = nullptr, QString* err = nullptr);

        // Read remote text file via SFTP (intended for small files like authorized_keys).
        bool readRemoteTextFile(const QString& remotePath, QString* textOut, QString* err = nullptr);

        // Write remote text atomically-ish via SFTP temp file + rename.
        // Ensures final permissions (chmod) as some servers ignore create perms.
        bool writeRemoteTextFileAtomic(const QString& remotePath, const QString& text, int permsOctal, QString* err = nullptr);

        // Ensure remote directory exists and apply permissions.
        bool ensureRemoteDir(const QString& path, int permsOctal, QString* err = nullptr);

        // Idempotent authorized_keys installer:
        //   - ensures ~/.ssh exists (0700)
        //   - backs up authorized_keys if it exists
        //   - appends key only if missing (no duplicates)
        //   - enforces authorized_keys perms (0600)
        //
        // alreadyOut:
        //   set true if key was already present (no change made)
        //
        // backupPathOut:
        //   returns remote path of the created backup (when applicable)
        bool installAuthorizedKey(const QString& pubKeyLine,
                                  QString* errOut,
                                  bool* alreadyOut,
                                  QString* backupPathOut = nullptr);

        void requestCancelTransfer();

    private:
        // Active libssh session used for SFTP and remote exec helpers.
        ssh_session m_session = nullptr;

        // Provided by UI; used when libssh asks for a passphrase.
        PassphraseProvider m_passphraseProvider;

        // Internal helper: call provider if installed.
        QString requestPassphrase(const QString& keyFile, bool *ok);
        std::atomic_bool m_cancelRequested{false};
    };
