// SshClient.h
#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

#include "SshProfile.h"
#include <sodium.h>           // if decrypt uses libsodium/wipe

struct ssh_session_struct; // forward declare libssh type
using ssh_session = ssh_session_struct*;



class SshClient : public QObject
{
    Q_OBJECT
public:
    explicit SshClient(QObject *parent = nullptr);
    ~SshClient() override;

    using PassphraseProvider = std::function<QString(const QString& keyFile, bool *ok)>;

    void setPassphraseProvider(PassphraseProvider cb) { m_passphraseProvider = std::move(cb); }

    bool connectProfile(const SshProfile& profile, QString* err = nullptr);
    bool connectPublicKey(const QString& target, QString* err = nullptr);
    bool testUnlockDilithiumKey(const QString& encKeyPath, QString* err = nullptr);

    void disconnect();
    bool isConnected() const;

    QString remotePwd(QString* err = nullptr) const;
    bool uploadBytes(const QString& remotePath, const QByteArray& data, QString* err = nullptr);
    bool downloadToFile(const QString& remotePath, const QString& localPath, QString* err = nullptr);



private:
    ssh_session m_session = nullptr;
    PassphraseProvider m_passphraseProvider;
    QString requestPassphrase(const QString& keyFile, bool *ok);
};
