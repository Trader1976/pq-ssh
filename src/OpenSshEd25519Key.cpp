#include "OpenSshEd25519Key.h"
#include <QRandomGenerator>

static void u32(QByteArray &b, quint32 v) {
    b.append(char((v >> 24) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

static void sshStr(QByteArray &b, const QByteArray &s) {
    u32(b, s.size());
    b.append(s);
}

static QByteArray pubBlob(const QByteArray &pub32) {
    QByteArray b;
    sshStr(b, "ssh-ed25519");
    sshStr(b, pub32);
    return b;
}

QString OpenSshEd25519Key::publicKeyLine(const QByteArray &pub32,
                                        const QString &comment)
{
    const QByteArray blob = pubBlob(pub32).toBase64();
    return QString("ssh-ed25519 %1 %2")
            .arg(QString::fromLatin1(blob),
                 comment.isEmpty() ? "pq-ssh" : comment);
}

QByteArray OpenSshEd25519Key::privateKeyFile(const QByteArray &pub32,
                                            const QByteArray &priv64,
                                            const QString &comment)
{
    QByteArray key;
    key.append("openssh-key-v1\0", 15);
    sshStr(key, "none");
    sshStr(key, "none");
    sshStr(key, {});
    u32(key, 1);

    QByteArray pb = pubBlob(pub32);
    sshStr(key, pb);

    QByteArray rec;
    quint32 chk = QRandomGenerator::global()->generate();
    u32(rec, chk); u32(rec, chk);
    sshStr(rec, "ssh-ed25519");
    sshStr(rec, pub32);
    sshStr(rec, priv64);
    const QByteArray cmt = comment.isEmpty() ? QByteArray("pq-ssh") : comment.toUtf8();
    sshStr(rec, cmt);

    // Pad with 1,2,3,... until size is a multiple of 8 (OpenSSH convention)
    int pad = 8 - (rec.size() % 8);
    if (pad == 8) pad = 0;
    for (int i = 1; i <= pad; ++i)
        rec.append(char(i));

    sshStr(key, rec);

    QByteArray b64 = key.toBase64();
    QByteArray out("-----BEGIN OPENSSH PRIVATE KEY-----\n");
    for (int i = 0; i < b64.size(); i += 70)
        out.append(b64.mid(i, 70)).append('\n');
    out.append("-----END OPENSSH PRIVATE KEY-----\n");
    return out;
}
