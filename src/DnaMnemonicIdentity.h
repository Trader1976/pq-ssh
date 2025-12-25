#pragma once
#include <QByteArray>
#include <QString>

struct DnaIdentityKeys {
    QString   fingerprintHex; // 128 hex chars (SHA3-512(pk))
    QByteArray dsaPk;          // ML-DSA-87 public key bytes (Dilithium5 pk)
    QByteArray dsaSk;          // ML-DSA-87 secret key bytes (Dilithium5 sk)
};

namespace DnaMnemonicIdentity {

    // Deterministically derives Dilithium (ML-DSA-87) keys + fingerprint from 24 words.
    // Passphrase is BIP39 passphrase (optional). Use "" for none.
    //
    // Returns empty fingerprint on failure.
    DnaIdentityKeys deriveDsa87From24Words(const QString &words24,
                                          const QString &passphrase);
}
