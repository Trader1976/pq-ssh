// SshClient.cpp
#include "SshClient.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>
#include <sys/stat.h>

static QString libsshError(ssh_session s)
{
    if (!s) return QStringLiteral("libssh: null session");
    return QString::fromLocal8Bit(ssh_get_error(s));
}

SshClient::SshClient() = default;

SshClient::~SshClient()
{
    disconnect();
}

bool SshClient::connectPublicKey(const QString& target, QString* err)
{
    if (err) err->clear();

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
        return false;
    }

    // Build a temporary profile (backwards compatible path)
    SshProfile p;
    p.host = host;
    p.user = user.isEmpty() ? qEnvironmentVariable("USER", "user") : user;
    p.port = 22;
    p.keyType = "auto";
    // p.keyFile left empty intentionally

    return connectProfile(p, err);
}


bool SshClient::connectProfile(const SshProfile& profile, QString* err)
{
    if (err) err->clear();

    const QString host = profile.host.trimmed();
    const QString user = profile.user.trimmed();

    if (host.isEmpty()) {
        if (err) *err = QStringLiteral("No host specified.");
        return false;
    }
    if (user.isEmpty()) {
        if (err) *err = QStringLiteral("No user specified.");
        return false;
    }

    // Key-type gate (PQ later)
    const QString kt = profile.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : profile.keyType.trimmed();
    if (kt != "auto" && kt != "openssh") {
        if (err) *err = QStringLiteral("Unsupported key_type '%1' (PQ keys not implemented yet).").arg(kt);
        return false;
    }

    // Clean previous session
    disconnect();

    ssh_session s = ssh_new();
    if (!s) {
        if (err) *err = QStringLiteral("ssh_new() failed.");
        return false;
    }

    // Options
    ssh_options_set(s, SSH_OPTIONS_HOST, host.toUtf8().constData());
    ssh_options_set(s, SSH_OPTIONS_USER, user.toUtf8().constData());

    const int port = (profile.port > 0) ? profile.port : 22;
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);

    // Optional: keep it responsive
    int timeoutSec = 8;
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeoutSec);

    // If a specific identity (private key) is set, tell libssh to use it
    if (!profile.keyFile.trimmed().isEmpty()) {
        const QByteArray p = QFile::encodeName(profile.keyFile.trimmed());
        ssh_options_set(s, SSH_OPTIONS_IDENTITY, p.constData());
    }

    // Connect
    int rc = ssh_connect(s);
    if (rc != SSH_OK) {
        if (err) *err = QStringLiteral("ssh_connect failed: %1").arg(libsshError(s));
        ssh_free(s);
        return false;
    }

    // Auth:
    // 1) try agent (nice if user has ssh-agent running)
    rc = ssh_userauth_agent(s, nullptr);

    // 2) then try publickey auto (uses SSH_OPTIONS_IDENTITY if set, else default keys)
    if (rc != SSH_AUTH_SUCCESS) {
        rc = ssh_userauth_publickey_auto(s, nullptr, nullptr);
    }

    if (rc != SSH_AUTH_SUCCESS) {
        if (err) *err = QStringLiteral("Public-key auth failed: %1").arg(libsshError(s));
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }

    m_session = s;
    return true;
}

void SshClient::disconnect()
{
    if (m_session) {
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
