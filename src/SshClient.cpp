
    // SshClient.cpp
    //
    // Purpose:
    //   Central SSH/SFTP utility layer for pq-ssh.
    //   - Creates and owns a libssh session
    //   - Authenticates with agent/public key (OpenSSH-compatible today)
    //   - Provides small “remote exec” helpers used by the UI
    //   - Provides SFTP upload/download helpers (streaming, cancelable, safe temp + rename)
    //   - Provides SFTP directory listing/stat helpers (file manager UI)
    //   - Implements idempotent authorized_keys installation (with backups)
    //
    // Design notes:
    //   - This is intentionally NOT a full “SSH terminal” implementation.
    //     The interactive terminal is handled via qtermwidget + spawning OpenSSH,
    //     while libssh is mainly used for SFTP and small command execution.
    //   - Never log secrets (passwords, passphrases, private key paths).

    #include "SshClient.h"

    #include <QFile>
    #include <QFileInfo>
    #include <QDir>
    #include <QDebug>
    #include <QRegularExpression>
    #include <QDateTime>

    #include <libssh/libssh.h>
    #include <libssh/sftp.h>
    #include <libssh/callbacks.h>

    #include <fcntl.h>
    #include <sys/stat.h>

    #include <sodium.h>
    #include <cstring>   // memset, memcpy
    #include <algorithm> // std::min, std::fill

    #include "DilithiumKeyCrypto.h"
    #include <QCryptographicHash>
    #include <QElapsedTimer>

    // ------------------------------------------------------------
    // Small helper to turn libssh's last error into QString
    // ------------------------------------------------------------
    static QString libsshError(ssh_session s)
    {
        if (!s) return QStringLiteral("libssh: null session");
        return QString::fromLocal8Bit(ssh_get_error(s));
    }

    // ------------------------------------------------------------
    // Small helper: open+init SFTP for current session
    // ------------------------------------------------------------
    static bool openSftp(ssh_session session, sftp_session *out, QString *err)
    {
        if (err) err->clear();
        if (!out) { if (err) *err = "openSftp: out is null."; return false; }
        *out = nullptr;

        if (!session) { if (err) *err = "Not connected."; return false; }

        sftp_session sftp = sftp_new(session);
        if (!sftp) { if (err) *err = "sftp_new failed."; return false; }

        if (sftp_init(sftp) != SSH_OK) {
            if (err) *err = "sftp_init failed: " + libsshError(session);
            sftp_free(sftp);
            return false;
        }

        *out = sftp;
        return true;
    }

    SshClient::SshClient(QObject *parent) : QObject(parent) {}

    SshClient::~SshClient()
    {
        // Ensure we never leak sessions on shutdown.
        disconnect();
    }

    // ------------------------------------------------------------
    // Cancel flag support (best-effort)
    // ------------------------------------------------------------
    void SshClient::requestCancelTransfer()
    {
        m_cancelRequested.store(true);
    }

    // ------------------------------------------------------------
    // Backwards-compatible entry point: connect using "user@host"
    // This builds a temporary SshProfile and forwards to connectProfile().
    // ------------------------------------------------------------
    bool SshClient::connectPublicKey(const QString& target, QString* err)
    {
        if (err) err->clear();

        qInfo().noquote() << QString("[SSH] connectPublicKey target='%1'").arg(target.trimmed());

        // Parse user@host (user optional)
        QString user;
        QString host = target.trimmed();
        const int atPos = host.indexOf('@');
        if (atPos >= 0) {
            user = host.left(atPos).trimmed();
            host = host.mid(atPos + 1).trimmed();
        }

        if (host.isEmpty()) {
            if (err) *err = QStringLiteral("No host specified.");
            qWarning().noquote() << QString("[SSH] connectPublicKey FAILED: %1")
                                    .arg(err ? *err : "No host specified.");
            return false;
        }

        // Build a temporary profile (backwards compatible path)
        SshProfile p;
        p.host = host;
        p.user = user.isEmpty() ? qEnvironmentVariable("USER", "user") : user;
        p.port = 22;
        p.keyType = "auto";

        const bool ok = connectProfile(p, err);

        if (ok) qInfo().noquote() << QString("[SSH] connectPublicKey OK user='%1' host='%2' port=%3")
                                     .arg(p.user, p.host).arg(p.port);
        else     qWarning().noquote() << QString("[SSH] connectPublicKey FAILED: %1")
                                     .arg(err ? *err : QString("unknown error"));

        return ok;
    }

    // ------------------------------------------------------------
    // Passphrase provider: injected from UI (MainWindow) so this
    // class stays UI-agnostic.
    // ------------------------------------------------------------
    QString SshClient::requestPassphrase(const QString& keyFile, bool *ok)
    {
        if (!m_passphraseProvider) {
            if (ok) *ok = false;
            return QString();
        }
        return m_passphraseProvider(keyFile, ok);
    }

    // ------------------------------------------------------------
    // DEV helper: verify that an encrypted Dilithium key can be unlocked
    // without revealing any plaintext.
    // ------------------------------------------------------------
    bool SshClient::testUnlockDilithiumKey(const QString& encKeyPath, QString* err)
    {
        if (err) err->clear();

        QFile f(encKeyPath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err) *err = QString("Cannot read key file: %1").arg(f.errorString());
            return false;
        }
        const QByteArray enc = f.readAll();
        f.close();

        if (!m_passphraseProvider) {
            if (err) *err = "No passphrase provider set (UI callback missing).";
            return false;
        }

        bool ok = false;
        const QString pass = m_passphraseProvider(encKeyPath, &ok);
        if (!ok) {
            if (err) *err = "Cancelled by user.";
            return false;
        }

        QByteArray plain;
        QString decErr;
        if (!decryptDilithiumKey(enc, pass, &plain, &decErr)) {
            if (err) *err = "Decrypt failed: " + decErr;
            return false;
        }

        if (plain.isEmpty()) {
            if (err) *err = "Decrypt returned empty plaintext.";
            return false;
        }

        // Best-effort wipe (plaintext should never persist longer than needed).
        if (sodium_init() >= 0) sodium_memzero(plain.data(), (size_t)plain.size());
        else std::fill(plain.begin(), plain.end(), '\0');

        return true;
    }



    // ------------------------------------------------------------
    // connectProfile(): establish libssh session suitable for SFTP + exec
    // ------------------------------------------------------------
bool SshClient::connectProfile(const SshProfile& profile, QString* err)
{
    if (err) err->clear();

    const QString host = profile.host.trimmed();
    const QString user = profile.user.trimmed();

    if (host.isEmpty()) {
        if (err) *err = QStringLiteral("No host specified.");
        qWarning().noquote() << QString("[SSH] connectProfile FAILED: %1").arg(err ? *err : "No host");
        return false;
    }
    if (user.isEmpty()) {
        if (err) *err = QStringLiteral("No user specified.");
        qWarning().noquote() << QString("[SSH] connectProfile FAILED host='%1': %2")
                                .arg(host, err ? *err : "No user");
        return false;
    }

    // Key-type gate:
    const QString kt = profile.keyType.trimmed().isEmpty()
                           ? QStringLiteral("auto")
                           : profile.keyType.trimmed();
    if (kt != "auto" && kt != "openssh") {
        if (err) *err = QStringLiteral("Unsupported key_type '%1' (PQ keys not implemented yet).").arg(kt);
        qWarning().noquote() << QString("[SSH] connectProfile FAILED user='%1' host='%2': %3")
                                .arg(user, host, err ? *err : "Unsupported key_type");
        return false;
    }

    const int port = (profile.port > 0) ? profile.port : 22;
    const bool hasIdentity = !profile.keyFile.trimmed().isEmpty();

    qInfo().noquote() << QString("[SSH] connectProfile start user='%1' host='%2' port=%3 keyType='%4'%5")
                         .arg(user, host)
                         .arg(port)
                         .arg(kt)
                         .arg(hasIdentity ? " identity=explicit" : "");

    // Always clean up any previous libssh session before reconnecting.
    if (m_session) {
        qInfo().noquote() << "[SSH] existing session present -> disconnecting before reconnect";
    }
    disconnect();

    ssh_session s = ssh_new();
    if (!s) {
        if (err) *err = QStringLiteral("ssh_new() failed.");
        qWarning().noquote() << QString("[SSH] connectProfile FAILED user='%1' host='%2': %3")
                                .arg(user, host, err ? *err : "ssh_new failed");
        return false;
    }

    auto failAndFree = [&](const QString &msg) -> bool {
        if (err) *err = msg;
        qWarning().noquote() << QString("[SSH] connectProfile FAILED user='%1' host='%2': %3")
                                .arg(user, host, msg);
        ssh_free(s);
        return false;
    };

    auto optSet = [&](enum ssh_options_e opt, const void *val, const char *what) -> bool {
        const int r = ssh_options_set(s, opt, val);
        if (r != SSH_OK) {
            qWarning().noquote() << QString("[SSH] ssh_options_set(%1) failed: %2")
                                    .arg(QString::fromLatin1(what),
                                         libsshError(s));
            return false;
        }
        return true;
    };

    // --- Small local helper: raw KEX -> pretty label for UI ---
    auto prettyKexName = [](const QString& raw) -> QString {
        const QString r = raw.trimmed();

        // Hybrid PQ KEX (OpenSSH naming)
        if (r.contains("mlkem768x25519", Qt::CaseInsensitive))
            return QStringLiteral("ML-KEM-768 + X25519 (Hybrid PQ)");
        if (r.contains("sntrup761x25519", Qt::CaseInsensitive))
            return QStringLiteral("sntrup761 + X25519 (Hybrid PQ)");

        // Classical/common ones
        if (r.contains("curve25519", Qt::CaseInsensitive))
            return QStringLiteral("X25519 (Curve25519)");

        // Fallback: return raw if we don't know it
        return r.isEmpty() ? QStringLiteral("unknown") : r;
    };

    // Options
    optSet(SSH_OPTIONS_HOST, host.toUtf8().constData(), "HOST");
    optSet(SSH_OPTIONS_USER, user.toUtf8().constData(), "USER");
    optSet(SSH_OPTIONS_PORT, &port, "PORT");

    int timeoutSec = 8;
    optSet(SSH_OPTIONS_TIMEOUT, &timeoutSec, "TIMEOUT");

    if (hasIdentity) {
        const QByteArray p = QFile::encodeName(profile.keyFile.trimmed());
        optSet(SSH_OPTIONS_IDENTITY, p.constData(), "IDENTITY");
    }

    // --- Prefer PQ hybrid KEX when available (libssh 0.11.x+) ---
        // --- Prefer PQ hybrid KEX (OpenSSH naming) ---
        // NOTE: must be set BEFORE ssh_connect().
        const char *kexPref =
            "sntrup761x25519-sha512@openssh.com,"
            "curve25519-sha256";

        const int kexRc = ssh_options_set(s, SSH_OPTIONS_KEY_EXCHANGE, kexPref);
        if (kexRc == SSH_OK) {
            qInfo().noquote() << QString("[SSH] KEX preference set: %1").arg(kexPref);
        } else {
            qInfo().noquote() << QString("[SSH] KEX preference not applied: %1").arg(libsshError(s));
        }

    if (kexRc == SSH_OK) {
        qInfo().noquote() << QString("[SSH] KEX preference set: %1").arg(kexPref);
    } else {
        qInfo().noquote() << QString("[SSH] KEX preference not applied: %1").arg(libsshError(s));
    }

    // Passphrase callback (UI supplies passphrase)
    static ssh_callbacks_struct cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.userdata = this;

    cb.auth_function = [](const char *prompt,
                          char *buf,
                          size_t len,
                          int echo,
                          int verify,
                          void *userdata) -> int
    {
        Q_UNUSED(prompt);
        Q_UNUSED(echo);
        Q_UNUSED(verify);

        auto *self = static_cast<SshClient*>(userdata);
        if (!self) return SSH_AUTH_DENIED;
        if (!self->m_passphraseProvider) return SSH_AUTH_DENIED;

        bool ok = false;
        const QString pass = self->m_passphraseProvider(QString(), &ok);
        if (!ok) return SSH_AUTH_DENIED;

        const QByteArray utf8 = pass.toUtf8();
        if (len == 0) return SSH_AUTH_DENIED;

        const size_t n = std::min(len - 1, static_cast<size_t>(utf8.size()));
        std::memcpy(buf, utf8.constData(), n);
        buf[n] = '\0';
        return SSH_AUTH_SUCCESS;
    };

    ssh_set_callbacks(s, &cb);

    // Network connect
    int rc = ssh_connect(s);
    if (rc != SSH_OK) {
        const QString e = libsshError(s);
        return failAndFree(QStringLiteral("ssh_connect failed: %1").arg(e));
    }

    qInfo().noquote() << QString("[SSH] ssh_connect OK host='%1' port=%2").arg(host).arg(port);

    // Negotiated algorithms (now valid)
    const char *kexAlgoC = ssh_get_kex_algo(s);
    const char *cipherInC  = ssh_get_cipher_in(s);   // server->client
    const char *cipherOutC = ssh_get_cipher_out(s);  // client->server

    const QString rawKex = kexAlgoC ? QString::fromLatin1(kexAlgoC) : QString();
    const QString pretty = prettyKexName(rawKex);

    qInfo().noquote() << QString("[SSH] negotiated kex='%1' cipher in='%2' out='%3'")
                         .arg(rawKex.isEmpty() ? "?" : rawKex,
                              cipherInC ? cipherInC : "?",
                              cipherOutC ? cipherOutC : "?");

    // Emit to UI (MainWindow can print something cool)
    emit kexNegotiated(pretty, rawKex);

    const bool pq = rawKex.contains("mlkem", Qt::CaseInsensitive) ||
                    rawKex.contains("sntrup", Qt::CaseInsensitive);
    qInfo().noquote() << QString("[SSH] PQ KEX: %1").arg(pq ? "YES" : "NO");

    // Authentication strategy:
    rc = ssh_userauth_agent(s, nullptr);
    if (rc == SSH_AUTH_SUCCESS) {
        qInfo().noquote() << QString("[SSH] auth OK via agent user='%1' host='%2'").arg(user, host);
    }

    if (rc != SSH_AUTH_SUCCESS) {
        qInfo().noquote() << QString("[SSH] auth via agent failed -> trying publickey_auto user='%1' host='%2'")
                             .arg(user, host);

        rc = ssh_userauth_publickey_auto(s, nullptr, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            qInfo().noquote() << QString("[SSH] auth OK via publickey_auto user='%1' host='%2'").arg(user, host);
        }
    }

    if (rc != SSH_AUTH_SUCCESS) {
        const QString e = libsshError(s);
        if (err) *err = QStringLiteral("Public-key auth failed: %1").arg(e);

        qWarning().noquote() << QString("[SSH] auth FAILED user='%1' host='%2' err='%3'")
                                .arg(user, host, e);

        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }

    // Success: keep session
    m_session = s;

    qInfo().noquote() << QString("[SSH] connectProfile OK user='%1' host='%2' port=%3")
                         .arg(user, host)
                         .arg(port);

    return true;
}

    // ------------------------------------------------------------
    // Disconnect and free session (safe to call multiple times).
    // ------------------------------------------------------------
    void SshClient::disconnect()
    {
        if (m_session) {
            qInfo().noquote() << "[SSH] disconnect (ssh_disconnect + free)";
            ssh_disconnect(m_session);
            ssh_free(m_session);
            m_session = nullptr;
        }
    }

    bool SshClient::isConnected() const
    {
        return m_session != nullptr;
    }

    // ------------------------------------------------------------
    // Remote pwd helper (executes `pwd` over a fresh channel).
    // ------------------------------------------------------------
    QString SshClient::remotePwd(QString* err) const
    {
        if (err) err->clear();

        if (!m_session) {
            if (err) *err = QStringLiteral("Not connected.");
            return QString();
        }

        ssh_channel ch = ssh_channel_new(m_session);
        if (!ch) {
            if (err) *err = QStringLiteral("ssh_channel_new failed.");
            return QString();
        }

        if (ssh_channel_open_session(ch) != SSH_OK) {
            if (err) *err = QStringLiteral("ssh_channel_open_session failed: %1").arg(libsshError(m_session));
            ssh_channel_free(ch);
            return QString();
        }

        if (ssh_channel_request_exec(ch, "pwd") != SSH_OK) {
            if (err) *err = QStringLiteral("ssh_channel_request_exec(pwd) failed: %1").arg(libsshError(m_session));
            ssh_channel_close(ch);
            ssh_channel_free(ch);
            return QString();
        }

        QByteArray buf;
        char tmp[256];
        int n = 0;
        while ((n = ssh_channel_read(ch, tmp, sizeof(tmp), 0)) > 0) {
            buf.append(tmp, n);
        }

        ssh_channel_send_eof(ch);
        ssh_channel_close(ch);
        ssh_channel_free(ch);

        const QString pwd = QString::fromUtf8(buf).trimmed();
        if (pwd.isEmpty()) {
            if (err) *err = QStringLiteral("Remote 'pwd' returned empty.");
        }
        return pwd;
    }

    // ------------------------------------------------------------
    // SFTP: list remote directory (non-recursive).
    // ------------------------------------------------------------
    bool SshClient::listRemoteDir(const QString& remotePath,
                                  QVector<RemoteEntry>* outItems,
                                  QString* err)
    {
        if (err) err->clear();
        if (outItems) outItems->clear();

        if (!m_session) {
            if (err) *err = "Not connected.";
            return false;
        }

        const QString path = remotePath.trimmed().isEmpty() ? QStringLiteral(".") : remotePath.trimmed();

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_dir dir = sftp_opendir(sftp, path.toUtf8().constData());
        if (!dir) {
            if (err) *err = QString("sftp_opendir failed for '%1': %2").arg(path, libsshError(m_session));
            sftp_free(sftp);
            return false;
        }

        QVector<RemoteEntry> items;

        while (true) {
            sftp_attributes a = sftp_readdir(sftp, dir);
            if (!a) break;

            const QString name = QString::fromUtf8(a->name ? a->name : "");
            if (name == "." || name == "..") {
                sftp_attributes_free(a);
                continue;
            }

            RemoteEntry e;
            e.name = name;
            e.fullPath = path.endsWith('/') ? (path + name) : (path + "/" + name);
            e.size  = (quint64)a->size;
            e.perms = (quint32)a->permissions;
            e.mtime = (qint64)a->mtime;

            e.isDir = ((a->permissions & S_IFMT) == S_IFDIR);

            items.push_back(e);
            sftp_attributes_free(a);
        }

        sftp_closedir(dir);
        sftp_free(sftp);

        if (outItems) *outItems = items;
        return true;
    }

    // ------------------------------------------------------------
    // SFTP: stat remote path (file or directory).
    // ------------------------------------------------------------
    bool SshClient::statRemotePath(const QString& remotePath,
                                   RemoteEntry* outInfo,
                                   QString* err)
    {
        if (err) err->clear();
        if (!outInfo) { if (err) *err = "statRemotePath: outInfo is null."; return false; }
        *outInfo = RemoteEntry{};

        if (!m_session) {
            if (err) *err = "Not connected.";
            return false;
        }
        if (remotePath.trimmed().isEmpty()) {
            if (err) *err = "Remote path is empty.";
            return false;
        }

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_attributes a = sftp_stat(sftp, remotePath.toUtf8().constData());
        if (!a) {
            if (err) *err = QString("sftp_stat failed for '%1': %2").arg(remotePath, libsshError(m_session));
            sftp_free(sftp);
            return false;
        }

        RemoteEntry e;
        QFileInfo fi(remotePath);
        e.name     = fi.fileName();
        e.fullPath = remotePath;
        e.size     = (quint64)a->size;
        e.perms    = (quint32)a->permissions;
        e.mtime    = (qint64)a->mtime;
        e.isDir    = ((a->permissions & S_IFMT) == S_IFDIR);

        sftp_attributes_free(a);
        sftp_free(sftp);

        *outInfo = e;
        return true;
    }

    // ------------------------------------------------------------
    // SFTP: streaming upload local file -> remote file.
    //   - writes to <remote>.pqssh.part
    //   - rename to final
    //   - cancel removes temp
    // ------------------------------------------------------------
    bool SshClient::uploadFile(const QString& localPath,
                               const QString& remotePath,
                               QString* err,
                               ProgressCb progress)
    {
        if (err) err->clear();
        m_cancelRequested.store(false);

        if (!m_session) {
            if (err) *err = "Not connected.";
            return false;
        }
        if (localPath.trimmed().isEmpty() || remotePath.trimmed().isEmpty()) {
            if (err) *err = "uploadFile: localPath/remotePath empty.";
            return false;
        }

        QFile in(localPath);
        if (!in.open(QIODevice::ReadOnly)) {
            if (err) *err = in.errorString();
            return false;
        }

        const quint64 totalSize = (quint64)in.size();

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        const QString tmpPath = remotePath + ".pqssh.part";

        sftp_file f = sftp_open(
            sftp,
            tmpPath.toUtf8().constData(),
            O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR
        );

        if (!f) {
            if (err) *err = QString("Cannot open remote temp file '%1': %2").arg(tmpPath, libsshError(m_session));
            sftp_free(sftp);
            return false;
        }

        QByteArray buf(64 * 1024, Qt::Uninitialized);
        quint64 sent = 0;

        while (!in.atEnd()) {

            if (m_cancelRequested.load()) {
                sftp_close(f);
                sftp_unlink(sftp, tmpPath.toUtf8().constData());
                sftp_free(sftp);
                if (err) *err = "Cancelled by user";
                return false;
            }

            const qint64 n = in.read(buf.data(), buf.size());
            if (n <= 0) break;

            const ssize_t w = sftp_write(f, buf.constData(), (size_t)n);
            if (w < 0) {
                if (err) *err = QString("SFTP write failed: %1").arg(libsshError(m_session));
                sftp_close(f);
                sftp_unlink(sftp, tmpPath.toUtf8().constData());
                sftp_free(sftp);
                return false;
            }

            sent += (quint64)w;
            if (progress) progress(sent, totalSize);
        }

        sftp_close(f);

        // Atomic-ish replace (handle servers that don't overwrite on rename)
        if (sftp_rename(sftp,
                        tmpPath.toUtf8().constData(),
                        remotePath.toUtf8().constData()) != SSH_OK) {

            // Try unlink destination then rename again
            sftp_unlink(sftp, remotePath.toUtf8().constData());

            if (sftp_rename(sftp,
                            tmpPath.toUtf8().constData(),
                            remotePath.toUtf8().constData()) != SSH_OK) {
                if (err) *err = QString("SFTP rename failed '%1' -> '%2': %3")
                                    .arg(tmpPath, remotePath, libsshError(m_session));
                sftp_unlink(sftp, tmpPath.toUtf8().constData());
                sftp_free(sftp);
                return false;
            }
        }

        sftp_free(sftp);
        return true;
    }

    // ------------------------------------------------------------
    // SFTP: streaming download remote file -> local file.
    //   - writes to <local>.pqssh.part then rename to final
    //   - creates local dirs
    //   - cancel removes temp
    // ------------------------------------------------------------
    bool SshClient::downloadFile(const QString& remotePath,
                                 const QString& localPath,
                                 QString* err,
                                 std::function<void(quint64 done, quint64 total)> progressCb)
    {
        if (err) err->clear();
        m_cancelRequested.store(false);

        if (!m_session) {
            if (err) *err = "Not connected.";
            return false;
        }
        if (remotePath.trimmed().isEmpty() || localPath.trimmed().isEmpty()) {
            if (err) *err = "downloadFile: remotePath/localPath empty.";
            return false;
        }

        // Ensure local directory exists
        QFileInfo li(localPath);
        if (!li.absolutePath().isEmpty())
            QDir().mkpath(li.absolutePath());

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_file f = sftp_open(
            sftp,
            remotePath.toUtf8().constData(),
            O_RDONLY,
            0
        );

        if (!f) {
            if (err) *err = QString("Cannot open remote file '%1': %2").arg(remotePath, libsshError(m_session));
            sftp_free(sftp);
            return false;
        }

        // Best-effort total size
        quint64 total = 0;
        if (auto *st = sftp_stat(sftp, remotePath.toUtf8().constData())) {
            total = (quint64)st->size;
            sftp_attributes_free(st);
        }

        const QString tmpLocal = localPath + ".pqssh.part";
        QFile out(tmpLocal);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err) *err = out.errorString();
            sftp_close(f);
            sftp_free(sftp);
            return false;
        }

        QByteArray buf(64 * 1024, Qt::Uninitialized);
        quint64 done = 0;

        while (true) {

            if (m_cancelRequested.load()) {
                out.close();
                out.remove();
                sftp_close(f);
                sftp_free(sftp);
                if (err) *err = "Cancelled by user";
                return false;
            }

            const ssize_t n = sftp_read(f, buf.data(), buf.size());
            if (n == 0)
                break; // EOF
            if (n < 0) {
                if (err) *err = QString("SFTP read failed: %1").arg(libsshError(m_session));
                out.close();
                out.remove();
                sftp_close(f);
                sftp_free(sftp);
                return false;
            }

            const qint64 w = out.write(buf.constData(), (qint64)n);
            if (w != (qint64)n) {
                if (err) *err = QString("Local write failed: %1").arg(out.errorString());
                out.close();
                out.remove();
                sftp_close(f);
                sftp_free(sftp);
                return false;
            }

            done += (quint64)n;

            if (progressCb)
                progressCb(done, total);
        }

        out.close();
        sftp_close(f);
        sftp_free(sftp);

        QFile::remove(localPath);
        if (!QFile::rename(tmpLocal, localPath)) {
            if (err) *err = "Failed to rename downloaded temp file to final path.";
            QFile::remove(tmpLocal);
            return false;
        }

        return true;
    }

    // ------------------------------------------------------------
    // Upload raw bytes to a remote file via SFTP.
    // ------------------------------------------------------------
    bool SshClient::uploadBytes(const QString& remotePath, const QByteArray& data, QString* err)
    {
        if (err) err->clear();

        if (!m_session) {
            if (err) *err = QStringLiteral("Not connected.");
            return false;
        }
        if (remotePath.isEmpty()) {
            if (err) *err = QStringLiteral("Remote path is empty.");
            return false;
        }

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_file f = sftp_open(
            sftp,
            remotePath.toUtf8().constData(),
            O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
        );

        if (!f) {
            if (err) *err = QStringLiteral("sftp_open failed for '%1'.").arg(remotePath);
            sftp_free(sftp);
            return false;
        }

        const char* ptr = data.constData();
        ssize_t remaining = data.size();
        while (remaining > 0) {
            ssize_t written = sftp_write(f, ptr, (size_t)remaining);
            if (written < 0) {
                if (err) *err = QStringLiteral("sftp_write failed for '%1'.").arg(remotePath);
                sftp_close(f);
                sftp_free(sftp);
                return false;
            }
            ptr += written;
            remaining -= written;
        }

        sftp_close(f);
        sftp_free(sftp);
        return true;
    }

    // ------------------------------------------------------------
    // Download remote file via SFTP and write to local file.
    // NOTE: reads whole file into memory (small files).
    // ------------------------------------------------------------
    bool SshClient::downloadToFile(const QString& remotePath, const QString& localPath, QString* err)
    {
        if (err) err->clear();

        if (!m_session) {
            if (err) *err = QStringLiteral("Not connected.");
            return false;
        }
        if (remotePath.isEmpty()) {
            if (err) *err = QStringLiteral("Remote path is empty.");
            return false;
        }
        if (localPath.isEmpty()) {
            if (err) *err = QStringLiteral("Local path is empty.");
            return false;
        }

        QFileInfo li(localPath);
        QDir().mkpath(li.absolutePath());

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_file f = sftp_open(
            sftp,
            remotePath.toUtf8().constData(),
            O_RDONLY,
            0
        );

        if (!f) {
            if (err) *err = QStringLiteral("sftp_open failed for '%1'.").arg(remotePath);
            sftp_free(sftp);
            return false;
        }

        QByteArray data;
        char buf[4096];
        int n = 0;
        while ((n = (int)sftp_read(f, buf, sizeof(buf))) > 0) {
            data.append(buf, n);
        }

        sftp_close(f);
        sftp_free(sftp);

        QFile out(localPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err) *err = QStringLiteral("Could not write local file '%1': %2").arg(localPath, out.errorString());
            return false;
        }
        out.write(data);
        out.close();

        return true;
    }


QByteArray SshClient::sha256LocalFile(const QString& localPath, QString* err) const
{
    if (err) err->clear();

    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QString("Cannot open local file for hashing: %1").arg(f.errorString());
        return {};
    }

    QCryptographicHash h(QCryptographicHash::Sha256);

    QByteArray buf(64 * 1024, Qt::Uninitialized);
    while (!f.atEnd()) {

        // Allow cancel to abort verification too
        if (m_cancelRequested.load()) {
            if (err) *err = "Cancelled by user";
            return {};
        }

        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0) {
            if (err) *err = QString("Local read failed while hashing: %1").arg(f.errorString());
            return {};
        }
        if (n == 0) break;

        h.addData(buf.constData(), (int)n);
    }

    return h.result(); // 32 bytes
}

QByteArray SshClient::sha256RemoteFile(const QString& remotePath, QString* err)
{
    if (err) err->clear();

    if (!m_session) {
        if (err) *err = "Not connected.";
        return {};
    }

    sftp_session sftp = nullptr;
    if (!openSftp(m_session, &sftp, err)) return {};

    sftp_file f = sftp_open(
        sftp,
        remotePath.toUtf8().constData(),
        O_RDONLY,
        0
    );

    if (!f) {
        if (err) *err = "Cannot open remote file for hashing";
        sftp_free(sftp);
        return {};
    }

    QCryptographicHash h(QCryptographicHash::Sha256);

    QByteArray buf(64 * 1024, Qt::Uninitialized);

    while (true) {

        if (m_cancelRequested.load()) {
            sftp_close(f);
            sftp_free(sftp);
            if (err) *err = "Cancelled by user";
            return {};
        }

        const ssize_t n = sftp_read(f, buf.data(), buf.size());
        if (n == 0) break;     // EOF
        if (n < 0) {
            if (err) *err = "SFTP read failed while hashing remote file";
            sftp_close(f);
            sftp_free(sftp);
            return {};
        }

        h.addData(buf.constData(), (int)n);
    }

    sftp_close(f);
    sftp_free(sftp);

    return h.result(); // 32 bytes
}

bool SshClient::verifyLocalVsRemoteSha256(const QString& localPath,
                                         const QString& remotePath,
                                         QString* err)
{
    if (err) err->clear();

    QString e1, e2;
    const QByteArray l = sha256LocalFile(localPath, &e1);
    if (l.isEmpty()) {
        if (err) *err = "Local SHA-256 failed: " + e1;
        return false;
    }

    const QByteArray r = sha256RemoteFile(remotePath, &e2);
    if (r.isEmpty()) {
        if (err) *err = "Remote SHA-256 failed: " + e2;
        return false;
    }

    if (l != r) {
        if (err) {
            *err = QString("Checksum mismatch (SHA-256)\nLocal : %1\nRemote: %2")
                .arg(QString::fromLatin1(l.toHex()))
                .arg(QString::fromLatin1(r.toHex()));
        }
        return false;
    }

    return true;
}

bool SshClient::verifyRemoteVsLocalSha256(const QString& remotePath,
                                         const QString& localPath,
                                         QString* err)
{
    if (err) err->clear();

    QString e1, e2;
    const QByteArray r = sha256RemoteFile(remotePath, &e1);
    if (r.isEmpty()) {
        if (err) *err = "Remote SHA-256 failed: " + e1;
        return false;
    }

    const QByteArray l = sha256LocalFile(localPath, &e2);
    if (l.isEmpty()) {
        if (err) *err = "Local SHA-256 failed: " + e2;
        return false;
    }

    if (l != r) {
        if (err) {
            *err = QString("Checksum mismatch (SHA-256)\nRemote: %1\nLocal : %2")
                .arg(QString::fromLatin1(r.toHex()))
                .arg(QString::fromLatin1(l.toHex()));
        }
        return false;
    }

    return true;
}






// ------------------------------------------------------------
// Execute a remote command and capture stdout/stderr.
// Returns false if exit status != 0.
// Overload supports timeout (ms). timeoutMs <= 0 means "no timeout".
// ------------------------------------------------------------
bool SshClient::exec(const QString& command, QString* out, QString* err)
{
    // Backward compatible: no timeout
    return exec(command, out, err, /*timeoutMs=*/0);
}

bool SshClient::exec(const QString& command, QString* out, QString* err, int timeoutMs)
{
    if (out) out->clear();
    if (err) err->clear();

    if (!m_session) {
        if (err) *err = "Not connected.";
        return false;
    }

    ssh_channel ch = ssh_channel_new(m_session);
    if (!ch) {
        if (err) *err = "ssh_channel_new failed.";
        return false;
    }

    auto cleanup = [&]() {
        // Be defensive: only close if it was opened
        if (ssh_channel_is_open(ch)) {
            ssh_channel_send_eof(ch);
            ssh_channel_close(ch);
        }
        ssh_channel_free(ch);
        ch = nullptr;
    };

    auto fail = [&](const QString& msg) -> bool {
        if (err) *err = msg;
        cleanup();
        return false;
    };

    if (ssh_channel_open_session(ch) != SSH_OK)
        return fail("ssh_channel_open_session failed: " + libsshError(m_session));

    if (ssh_channel_request_exec(ch, command.toUtf8().constData()) != SSH_OK)
        return fail("ssh_channel_request_exec failed: " + libsshError(m_session));

    QByteArray outBuf, errBuf;
    char buf[4096];

    QElapsedTimer timer;
    timer.start();

    auto readAvailable = [&](int isStderr) -> bool {
        while (true) {
            const int n = ssh_channel_read(ch, buf, sizeof(buf), isStderr);
            if (n == SSH_ERROR)
                return false;
            if (n <= 0)
                break;

            if (isStderr) errBuf.append(buf, n);
            else          outBuf.append(buf, n);

            // Keep draining quickly without waiting
            const int avail = ssh_channel_poll_timeout(ch, 0, isStderr);
            if (avail == SSH_ERROR)
                return false;
            if (avail <= 0)
                break;
        }
        return true;
    };

    while (true) {
        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            if (err) *err = QString("Remote command timed out after %1 ms.").arg(timeoutMs);
            cleanup();
            return false;
        }

        // Wait up to 50ms for stdout activity (this is our main "tick")
        const int availOut = ssh_channel_poll_timeout(ch, /*timeoutMs*/ 50, /*is_stderr*/ 0);
        if (availOut == SSH_ERROR)
            return fail("ssh_channel_poll_timeout(stdout) failed: " + libsshError(m_session));

        if (availOut > 0) {
            if (!readAvailable(/*isStderr*/0))
                return fail("ssh_channel_read(stdout) failed: " + libsshError(m_session));
        }

        // Always drain any stderr that is available (don't block on it)
        const int availErr = ssh_channel_poll_timeout(ch, /*timeoutMs*/ 0, /*is_stderr*/ 1);
        if (availErr == SSH_ERROR)
            return fail("ssh_channel_poll_timeout(stderr) failed: " + libsshError(m_session));

        if (availErr > 0) {
            if (!readAvailable(/*isStderr*/1))
                return fail("ssh_channel_read(stderr) failed: " + libsshError(m_session));
        }

        // Stop when remote EOF and nothing more buffered
        if (ssh_channel_is_eof(ch)) {
            const int drainOut = ssh_channel_poll_timeout(ch, 0, 0);
            const int drainErr = ssh_channel_poll_timeout(ch, 0, 1);
            if (drainOut == SSH_ERROR)
                return fail("ssh_channel_poll_timeout(stdout/drain) failed: " + libsshError(m_session));
            if (drainErr == SSH_ERROR)
                return fail("ssh_channel_poll_timeout(stderr/drain) failed: " + libsshError(m_session));

            if (drainOut > 0) {
                if (!readAvailable(0))
                    return fail("ssh_channel_read(stdout/drain) failed: " + libsshError(m_session));
            }
            if (drainErr > 0) {
                if (!readAvailable(1))
                    return fail("ssh_channel_read(stderr/drain) failed: " + libsshError(m_session));
            }

            // If nothing left, we’re done
            if (drainOut <= 0 && drainErr <= 0)
                break;
        }
    }

    // Close and read exit status
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);

    const int status = ssh_channel_get_exit_status(ch); // deprecated in 0.11, but still works
    ssh_channel_free(ch);
    ch = nullptr;

    if (out) *out = QString::fromUtf8(outBuf);

    if (status != 0) {
        if (err) {
            const QString e = QString::fromUtf8(errBuf).trimmed();
            *err = e.isEmpty()
                ? QString("Remote command failed (exit %1).").arg(status)
                : QString("Remote command failed (exit %1): %2").arg(status).arg(e);
        }
        return false;
    }

    // Optional: even on success, you might still want stderr text:
    // if (err && !errBuf.isEmpty()) *err = QString::fromUtf8(errBuf);

    return true;
}



    // ------------------------------------------------------------
    // Ensure remote directory exists and apply permissions.
    // ------------------------------------------------------------
    bool SshClient::ensureRemoteDir(const QString& path, int permsOctal, QString* err)
    {
        QString out;
        const QString cmd = QString("mkdir -p %1 && chmod %2 %1")
            .arg(path)
            .arg(QString::number(permsOctal, 8));
        return exec(cmd, &out, err);
    }

    // ------------------------------------------------------------
    // Read a remote text file via SFTP (small files).
    // ------------------------------------------------------------
    bool SshClient::readRemoteTextFile(const QString& remotePath, QString* textOut, QString* err)
    {
        if (textOut) textOut->clear();
        if (err) err->clear();
        if (!m_session) { if (err) *err = "Not connected."; return false; }

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_file f = sftp_open(sftp, remotePath.toUtf8().constData(), O_RDONLY, 0);
        if (!f) {
            if (err) *err = QString("sftp_open failed for '%1' (may not exist).").arg(remotePath);
            sftp_free(sftp);
            return false;
        }

        QByteArray data;
        char buf[4096];
        int n;
        while ((n = (int)sftp_read(f, buf, sizeof(buf))) > 0) {
            data.append(buf, n);
        }

        sftp_close(f);
        sftp_free(sftp);

        if (textOut) *textOut = QString::fromUtf8(data);
        return true;
    }

    // ------------------------------------------------------------
    // Atomic-ish remote write using SFTP temp + rename + chmod.
    // ------------------------------------------------------------
    bool SshClient::writeRemoteTextFileAtomic(const QString& remotePath, const QString& text, int permsOctal, QString* err)
    {
        if (err) err->clear();
        if (!m_session) { if (err) *err = "Not connected."; return false; }

        const QString tmpPath = remotePath + ".pqssh.tmp";

        sftp_session sftp = nullptr;
        if (!openSftp(m_session, &sftp, err)) return false;

        sftp_file f = sftp_open(
            sftp,
            tmpPath.toUtf8().constData(),
            O_WRONLY | O_CREAT | O_TRUNC,
            (mode_t)permsOctal
        );
        if (!f) {
            if (err) *err = QString("sftp_open failed for '%1'.").arg(tmpPath);
            sftp_free(sftp);
            return false;
        }

        const QByteArray bytes = text.toUtf8();
        const char* ptr = bytes.constData();
        ssize_t remaining = bytes.size();

        while (remaining > 0) {
            ssize_t written = sftp_write(f, ptr, (size_t)remaining);
            if (written < 0) {
                if (err) *err = QString("sftp_write failed for '%1'.").arg(tmpPath);
                sftp_close(f);
                sftp_free(sftp);
                return false;
            }

            ptr += written;
            remaining -= written;
        }

        sftp_close(f);

        if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {
            sftp_unlink(sftp, remotePath.toUtf8().constData());
            if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {
                if (err) *err = QString("sftp_rename failed for '%1' → '%2'.").arg(tmpPath, remotePath);
                sftp_free(sftp);
                return false;
            }
        }

        sftp_free(sftp);

        // Ensure perms (create perms are not always respected)
        QString out;
        const QString chmodCmd = QString("chmod %1 %2").arg(QString::number(permsOctal, 8), remotePath);
        exec(chmodCmd, &out, nullptr);

        return true;
    }

    // ------------------------------------------------------------
    // authorized_keys helpers
    // ------------------------------------------------------------
    static QString normalizeKeyLine(const QString& line)
    {
        QString s = line.trimmed();
        s.replace(QRegularExpression("\\s+"), " ");
        return s;
    }

    static bool looksLikeOpenSshPubKey(const QString& line)
    {
        const QString s = line.trimmed();
        return s.startsWith("ssh-ed25519 ") || s.startsWith("ssh-rsa ") ||
               s.startsWith("ecdsa-sha2-") || s.startsWith("sk-ssh-ed25519");
    }

    static QString shQuote(const QString& s)
    {
        QString out = s;
        out.replace("'", "'\"'\"'");
        return "'" + out + "'";
    }

    // ------------------------------------------------------------
    // installAuthorizedKey(): Idempotent authorized_keys installer.
    // ------------------------------------------------------------
    bool SshClient::installAuthorizedKey(const QString& pubKeyLine,
                                         QString* err,
                                         bool* alreadyPresent,
                                         QString* backupPathOut)
    {
        if (err) err->clear();
        if (alreadyPresent) *alreadyPresent = false;
        if (backupPathOut) backupPathOut->clear();

        if (!m_session) {
            if (err) *err = "Not connected.";
            return false;
        }

        const QString key = normalizeKeyLine(pubKeyLine);
        if (key.isEmpty()) {
            if (err) *err = "Public key line is empty.";
            return false;
        }
        if (!looksLikeOpenSshPubKey(key)) {
            if (err) *err = "Does not look like an OpenSSH public key line.";
            return false;
        }

        // Resolve remote $HOME
        QString homeOut, e;
        if (!exec("printf %s \"$HOME\"", &homeOut, &e)) {
            if (err) *err = "Failed to read remote $HOME: " + e;
            return false;
        }
        const QString home = homeOut.trimmed();
        if (home.isEmpty()) {
            if (err) *err = "Remote $HOME is empty.";
            return false;
        }

        const QString sshDir = home + "/.ssh";
        const QString akPath = sshDir + "/authorized_keys";

        // Ensure ~/.ssh exists and is 0700
        if (!ensureRemoteDir(sshDir, 0700, &e)) {
            if (err) *err = e;
            return false;
        }

        // Read existing authorized_keys if present
        QString existing, readErr;
        const bool hasExisting = readRemoteTextFile(akPath, &existing, &readErr);

        const QString normKey = normalizeKeyLine(key);

        if (hasExisting) {
            const QStringList lines = existing.split('\n', Qt::SkipEmptyParts);
            for (const QString& ln : lines) {
                if (normalizeKeyLine(ln) == normKey) {
                    if (alreadyPresent) *alreadyPresent = true;
                    return true;
                }
            }
        }

        // Backup existing file (strict)
        QString backupPath;
        if (hasExisting) {
            const QString backupDir = sshDir + "/pqssh_backups";
            if (!ensureRemoteDir(backupDir, 0700, &e)) {
                if (err) *err = "Failed to create backup dir: " + e;
                return false;
            }

            const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
            backupPath = backupDir + "/authorized_keys." + ts + ".bak";

            QString out, cpErr;
            const QString cpCmd = QString("cp -f -- %1 %2")
                                      .arg(shQuote(akPath), shQuote(backupPath));

            if (!exec(cpCmd, &out, &cpErr)) {
                const QString cpCmd2 = QString("cp -f %1 %2")
                                           .arg(shQuote(akPath), shQuote(backupPath));
                if (!exec(cpCmd2, &out, &cpErr)) {
                    if (err) *err = QString("Backup failed. Aborting install.\nTried:\n%1\n%2\nError: %3")
                                        .arg(cpCmd, cpCmd2, cpErr);
                    return false;
                }
            }

            exec(QString("chmod 600 %1").arg(shQuote(backupPath)), &out, nullptr);
            if (backupPathOut) *backupPathOut = backupPath;
        }

        // Append key with newline
        QString newText = existing;
        if (!newText.isEmpty() && !newText.endsWith('\n'))
            newText += "\n";
        newText += key;
        newText += "\n";

        // Write atomically and chmod 600
        if (!writeRemoteTextFileAtomic(akPath, newText, 0600, &e)) {
            if (!backupPath.isEmpty()) {
                QString out;
                exec(QString("cp -f %1 %2").arg(shQuote(backupPath), shQuote(akPath)), &out, nullptr);
                exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);
            }
            if (err) *err = e;
            return false;
        }

        QString out;
        exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);

        return true;
    }
    static QString prettyKexName(const QString& kex)
        {
            const QString s = kex.trimmed();

            if (s.startsWith("mlkem768x25519", Qt::CaseInsensitive)) {
                return "ML-KEM-768 + X25519 (Kyber-768 class) — " + s;
            }
            if (s.startsWith("sntrup761x25519", Qt::CaseInsensitive)) {
                return "sntrup761 + X25519 (NTRU Prime) — " + s;
            }
            if (s.startsWith("curve25519", Qt::CaseInsensitive)) {
                return "Classical ECDH: X25519 — " + s;
            }
            return s;
        }
