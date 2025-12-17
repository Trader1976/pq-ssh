// SshClient.cpp
//
// Purpose:
//   Central SSH/SFTP utility layer for pq-ssh.
//   - Creates and owns a libssh session
//   - Authenticates with agent/public key (OpenSSH-compatible today)
//   - Provides small “remote exec” helpers used by the UI
//   - Provides SFTP upload/download helpers
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

// ------------------------------------------------------------
// Small helper to turn libssh's last error into QString
// ------------------------------------------------------------
static QString libsshError(ssh_session s)
{
    if (!s) return QStringLiteral("libssh: null session");
    return QString::fromLocal8Bit(ssh_get_error(s));
}


void SshClient::requestCancelTransfer()
{
    m_cancelRequested.store(true);
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
    // NOTE: defaulting to port 22 and keyType auto keeps old behavior.
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
//
// What this function does NOT do:
//   - It does not open an interactive shell for the UI terminal.
//     (Terminal uses OpenSSH spawned in qtermwidget.)
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
    // Today we support OpenSSH-compatible keys only ("auto"/"openssh").
    // PQ key types are reserved for future expansion.
    const QString kt = profile.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : profile.keyType.trimmed();
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

    // --- libssh options ---
    ssh_options_set(s, SSH_OPTIONS_HOST, host.toUtf8().constData());
    ssh_options_set(s, SSH_OPTIONS_USER, user.toUtf8().constData());
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);

    // Keep it responsive: do not hang forever on broken hosts.
    int timeoutSec = 8;
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeoutSec);

    // Optional identity file (private key path).
    // IMPORTANT: do not log the path (may leak user info).
    if (hasIdentity) {
        const QByteArray p = QFile::encodeName(profile.keyFile.trimmed());
        ssh_options_set(s, SSH_OPTIONS_IDENTITY, p.constData());
    }

    // ------------------------------------------------------------
    // Passphrase callback (UI supplies passphrase)
    // ------------------------------------------------------------
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

        // No UI hook installed -> deny
        if (!self->m_passphraseProvider) {
            return SSH_AUTH_DENIED;
        }

        // Ask UI for a passphrase (UI may show a dialog).
        bool ok = false;
        const QString pass = self->m_passphraseProvider(QString(), &ok);
        if (!ok) return SSH_AUTH_DENIED;

        const QByteArray utf8 = pass.toUtf8();
        if (len == 0) return SSH_AUTH_DENIED;

        // Copy into libssh buffer (NUL-terminated).
        const size_t n = std::min(len - 1, static_cast<size_t>(utf8.size()));
        std::memcpy(buf, utf8.constData(), n);
        buf[n] = '\0';

        return SSH_AUTH_SUCCESS;
    };

    ssh_set_callbacks(s, &cb);

    // ------------------------------------------------------------
    // Network connect
    // ------------------------------------------------------------
    int rc = ssh_connect(s);
    if (rc != SSH_OK) {
        const QString e = libsshError(s);
        if (err) *err = QStringLiteral("ssh_connect failed: %1").arg(e);

        qWarning().noquote() << QString("[SSH] ssh_connect FAILED user='%1' host='%2' port=%3 err='%4'")
                                .arg(user, host)
                                .arg(port)
                                .arg(e);

        ssh_free(s);
        return false;
    }

    qInfo().noquote() << QString("[SSH] ssh_connect OK host='%1' port=%2").arg(host).arg(port);

    // ------------------------------------------------------------
    // Authentication strategy (best effort):
    //   1) Try SSH agent
    //   2) Try publickey_auto
    // ------------------------------------------------------------
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

    // Success: store session
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
// Used by drag-drop upload as a "where am I?" resolution.
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
        if (err) *err = QString("sftp_opendir failed for '%1'.").arg(path);
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

        // Determine directory bit from mode
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
        if (err) *err = QString("sftp_stat failed for '%1'.").arg(remotePath);
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
// ------------------------------------------------------------
bool SshClient::uploadFile(const QString& localPath,
                           const QString& remotePath,
                           QString *err,
                           std::function<void(quint64, quint64)> progressCb)
{
    if (err) err->clear();
    m_cancelRequested.store(false);

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (err) *err = in.errorString();
        return false;
    }

    const quint64 totalSize = in.size();

    sftp_session sftp = sftp_new(m_session);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        if (err) *err = "SFTP init failed";
        return false;
    }

    const QString tmpPath = remotePath + ".pqssh.part";

    sftp_file f = sftp_open(
        sftp,
        tmpPath.toUtf8().constData(),
        O_WRONLY | O_CREAT | O_TRUNC,
        S_IRUSR | S_IWUSR
    );

    if (!f) {
        if (err) *err = "Cannot open remote file";
        sftp_free(sftp);
        return false;
    }

    QByteArray buf(64 * 1024, Qt::Uninitialized);
    quint64 sent = 0;

    while (!in.atEnd()) {

        // 🔴 CANCEL CHECK
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
            if (err) *err = "SFTP write failed";
            sftp_close(f);
            sftp_unlink(sftp, tmpPath.toUtf8().constData());
            sftp_free(sftp);
            return false;
        }

        sent += w;
        if (progressCb) progressCb(sent, totalSize);
    }

    sftp_close(f);

    // Atomic replace
    sftp_rename(sftp,
                tmpPath.toUtf8().constData(),
                remotePath.toUtf8().constData());

    sftp_free(sftp);
    return true;
}
// ------------------------------------------------------------
// SFTP: streaming download remote file -> local file.
// ------------------------------------------------------------

bool SshClient::downloadFile(const QString& remotePath,
                             const QString& localPath,
                             QString *err,
                             std::function<void(quint64, quint64)> progressCb)
{
    if (err) err->clear();
    m_cancelRequested.store(false);

    sftp_session sftp = sftp_new(m_session);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        if (err) *err = "SFTP init failed";
        return false;
    }

    sftp_file f = sftp_open(
        sftp,
        remotePath.toUtf8().constData(),
        O_RDONLY,
        0
    );

    if (!f) {
        if (err) *err = "Cannot open remote file";
        sftp_free(sftp);
        return false;
    }

    // Best-effort total size
    quint64 total = 0;
    if (auto *st = sftp_stat(sftp, remotePath.toUtf8().constData())) {
        total = st->size;
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

        // 🔴 CANCEL SUPPORT
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
            if (err) *err = "SFTP read failed";
            out.close();
            out.remove();
            sftp_close(f);
            sftp_free(sftp);
            return false;
        }

        out.write(buf.constData(), n);
        done += n;

        if (progressCb)
            progressCb(done, total);
    }

    out.close();
    sftp_close(f);
    sftp_free(sftp);

    QFile::remove(localPath);
    QFile::rename(tmpLocal, localPath);

    return true;
}



// ------------------------------------------------------------
// Upload raw bytes to a remote file via SFTP.
// Creates/truncates the target and writes all data.
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

    // Mode: 0644 by default (owner rw, group r, others r)
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
// Simple implementation reads whole file into memory.
// NOTE: prefer downloadFile() for large files.
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

    // Ensure local directory exists
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

// ------------------------------------------------------------
// Execute a remote command and capture stdout/stderr.
// Returns false if exit status != 0 (or if transport fails).
//
// SECURITY NOTE:
//   This is used for small controlled commands (mkdir/chmod/cp/etc).
//   Callers must avoid passing untrusted user input without quoting.
// ------------------------------------------------------------
bool SshClient::exec(const QString& command, QString* out, QString* err)
{
    if (out) out->clear();
    if (err) err->clear();
    if (!m_session) { if (err) *err = "Not connected."; return false; }

    ssh_channel ch = ssh_channel_new(m_session);
    if (!ch) { if (err) *err = "ssh_channel_new failed."; return false; }

    if (ssh_channel_open_session(ch) != SSH_OK) {
        if (err) *err = "ssh_channel_open_session failed: " + libsshError(m_session);
        ssh_channel_free(ch);
        return false;
    }

    if (ssh_channel_request_exec(ch, command.toUtf8().constData()) != SSH_OK) {
        if (err) *err = "ssh_channel_request_exec failed: " + libsshError(m_session);
        ssh_channel_close(ch);
        ssh_channel_free(ch);
        return false;
    }

    QByteArray outBuf, errBuf;
    char buf[4096];
    int n;

    while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) {
        outBuf.append(buf, n);
    }
    while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) {
        errBuf.append(buf, n);
    }

    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);

    const int status = ssh_channel_get_exit_status(ch);
    ssh_channel_free(ch);

    if (out) *out = QString::fromUtf8(outBuf);
    if (status != 0) {
        if (err) {
            const QString e = QString::fromUtf8(errBuf).trimmed();
            *err = e.isEmpty() ? QString("Remote command failed (exit %1).").arg(status)
                               : QString("Remote command failed (exit %1): %2").arg(status).arg(e);
        }
        return false;
    }

    return true;
}

// ------------------------------------------------------------
// Ensure remote directory exists and apply permissions.
// Used by key install workflow for ~/.ssh and backup dir.
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
// Read a remote text file via SFTP. Intended for small files.
// If file does not exist, returns false with a helpful error.
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
// Atomic-ish remote write using SFTP:
//   1) write to <file>.pqssh.tmp
//   2) rename tmp -> final (may require unlink final first)
//   3) chmod to ensure final perms
//
// Rationale:
//   authorized_keys must never be partially written.
// ------------------------------------------------------------
bool SshClient::writeRemoteTextFileAtomic(const QString& remotePath, const QString& text, int permsOctal, QString* err)
{
    if (err) err->clear();
    if (!m_session) { if (err) *err = "Not connected."; return false; }

    const QString tmpPath = remotePath + ".pqssh.tmp";

    sftp_session sftp = nullptr;
    if (!openSftp(m_session, &sftp, err)) return false;

    // NOTE: permsOctal is passed as-is; servers may still ignore it.
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

    // Try rename tmp -> target
    if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {

        // Some servers disallow overwrite on rename.
        // Try unlinking destination first (best-effort).
        sftp_unlink(sftp, remotePath.toUtf8().constData());

        if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {
            if (err) *err = QString("sftp_rename failed for '%1' → '%2' (server may not allow overwrite).")
                                .arg(tmpPath, remotePath);
            sftp_free(sftp);
            return false;
        }
    }

    sftp_free(sftp);

    // Ensure perms (create perms are not always respected by SFTP servers).
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
    // Normalize whitespace so duplicate detection is robust.
    QString s = line.trimmed();
    s.replace(QRegularExpression("\\s+"), " ");
    return s;
}

static bool looksLikeOpenSshPubKey(const QString& line)
{
    // Lightweight check: we only accept OpenSSH-style public key lines.
    // (We don't fully parse base64 here; we just reject obvious garbage.)
    const QString s = line.trimmed();
    return s.startsWith("ssh-ed25519 ") || s.startsWith("ssh-rsa ") ||
           s.startsWith("ecdsa-sha2-") || s.startsWith("sk-ssh-ed25519");
}

static QString shQuote(const QString& s)
{
    // POSIX-safe single-quote escaping:
    //   abc'd  ->  'abc'"'"'d'
    QString out = s;
    out.replace("'", "'\"'\"'");
    return "'" + out + "'";
}

// ------------------------------------------------------------
// installAuthorizedKey():
//   Idempotent authorized_keys installer.
//
// Guarantees:
//   - ~/.ssh exists with 0700
//   - If authorized_keys exists, it is backed up first
//   - Key is appended only if missing (no duplicates)
//   - Final authorized_keys perms forced to 0600
//
// Security notes:
//   - Avoids modifying sshd_config
//   - Works with standard OpenSSH server expectations
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

    // 1) Resolve remote $HOME safely (avoid assuming /home/user).
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

    // 2) Ensure ~/.ssh exists and is 0700 (OpenSSH requirement).
    if (!ensureRemoteDir(sshDir, 0700, &e)) {
        if (err) *err = e;
        return false;
    }

    // 3) Read existing authorized_keys (if present).
    //    If it doesn't exist, readRemoteTextFile returns false and we treat it as new file.
    QString existing, readErr;
    const bool hasExisting = readRemoteTextFile(akPath, &existing, &readErr);

    const QString normKey = normalizeKeyLine(key);

    // Duplicate check (line-based compare after normalization).
    if (hasExisting) {
        const QStringList lines = existing.split('\n', Qt::SkipEmptyParts);
        for (const QString& ln : lines) {
            if (normalizeKeyLine(ln) == normKey) {
                if (alreadyPresent) *alreadyPresent = true;
                return true; // already installed, nothing to do
            }
        }
    }

    // 4) Backup existing file (STRICT: if file exists, backup must succeed).
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

        // Prefer cp -- to avoid option-injection on weird filenames.
        const QString cpCmd = QString("cp -f -- %1 %2")
                                  .arg(shQuote(akPath), shQuote(backupPath));

        if (!exec(cpCmd, &out, &cpErr)) {
            // Fallback for environments where "cp --" is unsupported.
            const QString cpCmd2 = QString("cp -f %1 %2")
                                       .arg(shQuote(akPath), shQuote(backupPath));

            if (!exec(cpCmd2, &out, &cpErr)) {
                if (err) *err = QString("Backup failed. Aborting install.\nTried:\n%1\n%2\nError: %3")
                                    .arg(cpCmd, cpCmd2, cpErr);
                return false;
            }
        }

        // Backups should not be world-readable.
        exec(QString("chmod 600 %1").arg(shQuote(backupPath)), &out, nullptr);

        if (backupPathOut) *backupPathOut = backupPath;
    }

    // 5) Append key with newline.
    QString newText = existing;
    if (!newText.isEmpty() && !newText.endsWith('\n'))
        newText += "\n";
    newText += key;
    newText += "\n";

    // 6) Write atomically and chmod 600.
    if (!writeRemoteTextFileAtomic(akPath, newText, 0600, &e)) {

        // If write failed after we backed up, attempt rollback (best-effort).
        if (!backupPath.isEmpty()) {
            QString out;
            exec(QString("cp -f %1 %2").arg(shQuote(backupPath), shQuote(akPath)), &out, nullptr);
            exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);
        }

        if (err) *err = e;
        return false;
    }

    // Extra safety: sshd can be picky if perms drift.
    QString out;
    exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);

    return true;
}
