// SshClient.h
#pragma once
#include <QString>
#include <QByteArray>

struct ssh_session_struct; // forward declare libssh type
using ssh_session = ssh_session_struct*;

class SshClient {
public:
    SshClient();
    ~SshClient();

    bool connectPublicKey(const QString& target, QString* err=nullptr);
    void disconnect();
    bool isConnected() const;

    QString remotePwd(QString* err=nullptr) const;
    bool uploadBytes(const QString& remotePath, const QByteArray& data, QString* err=nullptr);
    bool downloadToFile(const QString& remotePath, const QString& localPath, QString* err=nullptr);

private:
    ssh_session m_session = nullptr;
};
