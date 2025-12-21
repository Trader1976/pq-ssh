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
#include <QtGlobal>      // for quint64/quint32/qint64
#include <functional>
#include <atomic>

#include "SshProfile.h"

// Forward-declare libssh session type to avoid pulling libssh headers into the header.
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

signals:
    // Emitted after a successful ssh_connect(), when we can read negotiated KEX.
    void kexNegotiated(const QString& prettyText, const QString& rawKex);

public:
    // Register passphrase provider (typically set by MainWindow).
    void setPassphraseProvider(PassphraseProvider cb) { m_passphraseProvider = std::move(cb); }

    // Connect using a profile (preferred modern path).
    // Supports "auto"/"openssh" key_type today.
    // On success, m_session becomes valid and SFTP/exec helpers can be used.
    bool connectProfile(const SshProfile& profile, QString* err = nullptr);

    // Backwards-compatible helper: connect using "user@host" string.
    bool connectPublicKey(const QString& target, QString* err = nullptr);

    // DEV helper: attempt to decrypt an encrypted Dilithium key blob (no plaintext exposure).
    bool testUnlockDilithiumKey(const QString& encKeyPath, QString* err = nullptr);

    // Close/free current libssh session (safe to call multiple times).
    void disconnect();

    // True if a libssh session is active.
    bool isConnected() const;

    // Run remote `pwd` and return its trimmed output.
    QString remotePwd(QString* err = nullptr) const;

    // ------------------------------------------------------------
    // SFTP: file manager primitives
    // ------------------------------------------------------------
    bool listRemoteDir(const QString& remotePath,
                       QVector<RemoteEntry>* outItems,
                       QString* err = nullptr);

    bool statRemotePath(const QString& remotePath,
                        RemoteEntry* outInfo,
                        QString* err = nullptr);

    bool uploadFile(const QString& localPath,
                    const QString& remotePath,
                    QString* err = nullptr,
                    ProgressCb progress = nullptr);

    // --- Integrity / checksum (SHA-256) ---
    QByteArray sha256LocalFile(const QString& localPath, QString* err = nullptr) const;
    QByteArray sha256RemoteFile(const QString& remotePath, QString* err = nullptr);

    bool verifyLocalVsRemoteSha256(const QString& localPath,
                                   const QString& remotePath,
                                   QString* err = nullptr);

    bool verifyRemoteVsLocalSha256(const QString& remotePath,
                                   const QString& localPath,
                                   QString* err = nullptr);

    bool downloadFile(const QString& remotePath,
                      const QString& localPath,
                      QString* err = nullptr,
                      ProgressCb progress = nullptr);

    // ------------------------------------------------------------
    // Existing helpers (kept for compatibility / small-file use)
    // ------------------------------------------------------------
    bool uploadBytes(const QString& remotePath, const QByteArray& data, QString* err = nullptr);
    bool downloadToFile(const QString& remotePath, const QString& localPath, QString* err = nullptr);

    bool exec(const QString& command, QString* out = nullptr, QString* err = nullptr);
    // New overload
    bool exec(const QString& command, QString* out, QString* err, int timeoutMs);

    bool readRemoteTextFile(const QString& remotePath, QString* textOut, QString* err = nullptr);

    bool writeRemoteTextFileAtomic(const QString& remotePath,
                                   const QString& text,
                                   int permsOctal,
                                   QString* err = nullptr);

    bool ensureRemoteDir(const QString& path, int permsOctal, QString* err = nullptr);

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
