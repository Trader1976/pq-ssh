#pragma once
#include <QByteArray>
#include <QString>

namespace DnaIdentityDerivation {

    struct DerivedDnaIdentity {
        QByteArray masterSeed64;     // optional (caller may clear immediately)
        QByteArray signingSeed32;    // 32
        QByteArray encryptionSeed32; // 32 (optional)
        QByteArray dilithiumPk;      // ML-DSA-87 public
        QByteArray dilithiumSk;      // ML-DSA-87 private
        QString fingerprintHex128;   // SHA3-512(pk) hex (128 chars)
        QString error;
        bool ok = false;
    };

    // Normalization compatible with your current pq-ssh approach.
    // (True BIP39 uses NFKD; you can add later if needed.)
    QString normalizeMnemonic(const QString& words);

    // BIP39-style seed: PBKDF2-HMAC-SHA512(password=mnemonic,
    // salt="mnemonic"+passphrase, iter=2048, out=64)
    QByteArray bip39Seed64(const QString& mnemonicWords,
                           const QString& passphrase);

    // If you want a clearer name for UI usage, keep this as an alias.
    // (Implementation can just call bip39Seed64.)
    QByteArray bip39MasterSeed64(const QString& mnemonicWords,
                                 const QString& passphrase);

    // DNA seed derivation helper: SHAKE256(input, 32)
    QByteArray shake256_32(const QByteArray& input);

    // SHA3-512 hex (128 chars)
    QString sha3_512_hex(const QByteArray& bytes);

    // End-to-end: mnemonic -> seeds -> Dilithium keypair -> fingerprint
    DerivedDnaIdentity deriveFromWords(const QString& mnemonicWords,
                                       const QString& passphrase);

} // namespace DnaIdentityDerivation
