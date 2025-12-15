// SshClient.cpp
#include "SshClient.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/callbacks.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sodium.h>
#include <cstring>   // memset, memcpy
#include "DilithiumKeyCrypto.h"

static QString libsshError(ssh_session s)
{
    if (!s) return QStringLiteral("libssh: null session");
    return QString::fromLocal8Bit(ssh_get_error(s));
}

SshClient::SshClient(QObject *parent) : QObject(parent) {}

SshClient::~SshClient()
{
    disconnect();
}

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
        qWarning().noquote() << QString("[SSH] connectPublicKey FAILED: %1").arg(err ? *err : "No host specified.");
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



QString SshClient::requestPassphrase(const QString& keyFile, bool *ok)
{
    if (!m_passphraseProvider) {
        if (ok) *ok = false;
        return QString();
    }
    return m_passphraseProvider(keyFile, ok);
}

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

    // best-effort wipe
    if (sodium_init() >= 0) sodium_memzero(plain.data(), (size_t)plain.size());
    else std::fill(plain.begin(), plain.end(), '\0');

    return true;
}



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
        qWarning().noquote() << QString("[SSH] connectProfile FAILED host='%1': %2").arg(host, err ? *err : "No user");
        return false;
    }

    // Key-type gate (PQ later)
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

    // Clean previous session
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

    // Options
    ssh_options_set(s, SSH_OPTIONS_HOST, host.toUtf8().constData());
    ssh_options_set(s, SSH_OPTIONS_USER, user.toUtf8().constData());
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);

    // Optional: keep it responsive
    int timeoutSec = 8;
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeoutSec);

    // If a specific identity (private key) is set, tell libssh to use it
    if (hasIdentity) {
        const QByteArray p = QFile::encodeName(profile.keyFile.trimmed());
        ssh_options_set(s, SSH_OPTIONS_IDENTITY, p.constData());
        // NOTE: do NOT log the key path (can be sensitive)
    }

    // ------------------------------------------------------------
    // Option A: Passphrase callback (UI provides passphrase)
    // ------------------------------------------------------------
    // IMPORTANT: the callbacks struct must stay alive while session is alive.
    // We'll keep it static because PQ-SSH uses one SshClient instance / one session at a time.
    static ssh_callbacks_struct cb;
    std::memset(&cb, 0, sizeof(cb));

    cb.userdata = this;

    // libssh will call this when it needs a passphrase for a private key
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

        if (!self->m_passphraseProvider) {
            return SSH_AUTH_DENIED; // no UI hook installed
        }

        bool ok = false;

        // We don't get the key path reliably from libssh here; pass empty.
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
    // ------------------------------------------------------------

    // Connect
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

    // Auth:
    // 1) try agent (nice if user has ssh-agent running)
    rc = ssh_userauth_agent(s, nullptr);
    if (rc == SSH_AUTH_SUCCESS) {
        qInfo().noquote() << QString("[SSH] auth OK via agent user='%1' host='%2'").arg(user, host);
    }

    // 2) then try publickey auto (uses SSH_OPTIONS_IDENTITY if set, else default keys)
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

    m_session = s;

    qInfo().noquote() << QString("[SSH] connectProfile OK user='%1' host='%2' port=%3")
                         .arg(user, host)
                         .arg(port);

    return true;
}

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

    sftp_session sftp = sftp_new(m_session);
    if (!sftp) {
        if (err) *err = QStringLiteral("sftp_new failed.");
        return false;
    }
    if (sftp_init(sftp) != SSH_OK) {
        if (err) *err = QStringLiteral("sftp_init failed: %1").arg(libsshError(m_session));
        sftp_free(sftp);
        return false;
    }

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

    // Ensure local folder exists
    QFileInfo li(localPath);
    QDir().mkpath(li.absolutePath());

    sftp_session sftp = sftp_new(m_session);
    if (!sftp) {
        if (err) *err = QStringLiteral("sftp_new failed.");
        return false;
    }
    if (sftp_init(sftp) != SSH_OK) {
        if (err) *err = QStringLiteral("sftp_init failed: %1").arg(libsshError(m_session));
        sftp_free(sftp);
        return false;
    }

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
