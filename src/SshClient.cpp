// SshClient.cpp
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

        if (!self->m_passphraseProvider) {
            return SSH_AUTH_DENIED; // no UI hook installed
        }

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

bool SshClient::ensureRemoteDir(const QString& path, int permsOctal, QString* err)
{
    QString out;
    const QString cmd = QString("mkdir -p %1 && chmod %2 %1")
        .arg(path)
        .arg(QString::number(permsOctal, 8));
    return exec(cmd, &out, err);
}

bool SshClient::readRemoteTextFile(const QString& remotePath, QString* textOut, QString* err)
{
    if (textOut) textOut->clear();
    if (err) err->clear();
    if (!m_session) { if (err) *err = "Not connected."; return false; }

    sftp_session sftp = sftp_new(m_session);
    if (!sftp) { if (err) *err = "sftp_new failed."; return false; }
    if (sftp_init(sftp) != SSH_OK) {
        if (err) *err = "sftp_init failed: " + libsshError(m_session);
        sftp_free(sftp);
        return false;
    }

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

bool SshClient::writeRemoteTextFileAtomic(const QString& remotePath, const QString& text, int permsOctal, QString* err)
{
    if (err) err->clear();
    if (!m_session) { if (err) *err = "Not connected."; return false; }

    const QString tmpPath = remotePath + ".pqssh.tmp";

    sftp_session sftp = sftp_new(m_session);
    if (!sftp) { if (err) *err = "sftp_new failed."; return false; }
    if (sftp_init(sftp) != SSH_OK) {
        if (err) *err = "sftp_init failed: " + libsshError(m_session);
        sftp_free(sftp);
        return false;
    }

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

    // Atomic-ish replace: rename tmp -> target
    if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {

        // Many servers refuse rename-overwrite. Try unlink dest then rename again.
        sftp_unlink(sftp, remotePath.toUtf8().constData());

        if (sftp_rename(sftp, tmpPath.toUtf8().constData(), remotePath.toUtf8().constData()) != SSH_OK) {
            if (err) *err = QString("sftp_rename failed for '%1' → '%2' (server may not allow overwrite).")
                                .arg(tmpPath, remotePath);
            sftp_free(sftp);
            return false;
        }
    }

    sftp_free(sftp);

    // Ensure perms (some servers ignore perms on create)
    QString out;
    const QString chmodCmd = QString("chmod %1 %2").arg(QString::number(permsOctal, 8), remotePath);
    exec(chmodCmd, &out, nullptr);

    return true;
}

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
    // POSIX-safe single-quote escaping:  abc'd  ->  'abc'"'"'d'
    QString out = s;
    out.replace("'", "'\"'\"'");
    return "'" + out + "'";
}

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

    // 1) Resolve remote $HOME safely
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

    // 2) Ensure ~/.ssh exists and is 0700
    if (!ensureRemoteDir(sshDir, 0700, &e)) {
        if (err) *err = e;
        return false;
    }

    // 3) Read existing authorized_keys (if present)
    QString existing, readErr;
    const bool hasExisting = readRemoteTextFile(akPath, &existing, &readErr);

    const QString normKey = normalizeKeyLine(key);

    // Duplicate check (line-based)
    if (hasExisting) {
        const QStringList lines = existing.split('\n', Qt::SkipEmptyParts);
        for (const QString& ln : lines) {
            if (normalizeKeyLine(ln) == normKey) {
                if (alreadyPresent) *alreadyPresent = true;
                return true; // already installed
            }
        }
    }

    // 4) Backup existing file (STRICT: if file exists, backup must succeed)
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

        // ✅ expose the backup path to caller (same behavior for menu + key generator)
        if (backupPathOut) *backupPathOut = backupPath;
    }

    // 5) Append key with newline
    QString newText = existing;
    if (!newText.isEmpty() && !newText.endsWith('\n'))
        newText += "\n";
    newText += key;
    newText += "\n";

    // 6) Write atomically and chmod 600
    if (!writeRemoteTextFileAtomic(akPath, newText, 0600, &e)) {
        // If we created a backup and write failed, try to restore backup best-effort
        if (!backupPath.isEmpty()) {
            QString out;
            exec(QString("cp -f %1 %2").arg(shQuote(backupPath), shQuote(akPath)), &out, nullptr);
            exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);
        }

        if (err) *err = e;
        return false;
    }

    // Extra safety: sshd can be picky if perms drift
    QString out;
    exec(QString("chmod 600 %1").arg(shQuote(akPath)), &out, nullptr);

    return true;
}

