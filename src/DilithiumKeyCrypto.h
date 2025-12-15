#pragma once
#include <QByteArray>
#include <QString>


bool encryptDilithiumKey(
    const QByteArray& plain,
    const QString& passphrase,
    QByteArray* outEncrypted,
    QString* err
);

bool decryptDilithiumKey(
    const QByteArray& encrypted,
    const QString& passphrase,
    QByteArray* outPlain,
    QString* err
);
