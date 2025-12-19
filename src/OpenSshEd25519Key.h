#pragma once
#include <QByteArray>
#include <QString>

namespace OpenSshEd25519Key {

    QString publicKeyLine(const QByteArray &pub32,
                          const QString &comment);

    QByteArray privateKeyFile(const QByteArray &pub32,
                              const QByteArray &priv64,
                              const QString &comment);

}
