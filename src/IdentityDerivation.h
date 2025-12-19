#pragma once
#include <QByteArray>
#include <QString>

struct DerivedEd25519 {
    QByteArray seed32;   // Ed25519 seed
    QByteArray pub32;    // Ed25519 public key
    QByteArray priv64;   // seed || pub (OpenSSH expects this)
};

namespace IdentityDerivation {

    QByteArray bip39Seed64(const QString &mnemonicWords,
                           const QString &passphrase);

    QByteArray hkdfSha512_32(const QByteArray &ikm,
                             const QByteArray &info);

    DerivedEd25519 deriveEd25519FromWords(const QString &mnemonicWords,
                                          const QString &passphrase,
                                          const QString &infoString);

} // namespace IdentityDerivation
