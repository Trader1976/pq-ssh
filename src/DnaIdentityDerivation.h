#pragma once
#include <QByteArray>
#include <QString>

struct DerivedDnaIdentity {
    QByteArray masterSeed64;     // optional (can be cleared immediately)
    QByteArray signingSeed32;    // 32
    QByteArray encryptionSeed32; // 32 (if you want it)
    QByteArray dilithiumPk;      // 2592 (ML-DSA-87 public)
    QByteArray dilithiumSk;      // depends on your impl (often 4896 for Dilithium5)
    QString fingerprintHex128;   // SHA3-512(pk) hex
    QString error;
    bool ok = false;
};

namespace DnaIdentityDerivation {

    // Normalization compatible with your current pq-ssh approach.
    // (DNA/BIP39 “true” spec uses NFKD; you can add later if needed.)
    QString normalizeMnemonic(const QString& words);

    // BIP39-style seed: PBKDF2-HMAC-SHA512(password=mnemonic, salt="mnemonic"+passphrase, iter=2048, out=64)
    QByteArray bip39Seed64(const QString& mnemonicWords,
                           const QString& passphrase);

    // DNA seed derivation: SHAKE256(masterSeed||context, 32)
    QByteArray shake256_32(const QByteArray& input);

    // SHA3-512 hex (128 chars)
    QString sha3_512_hex(const QByteArray& bytes);

    // End-to-end: mnemonic -> seeds -> Dilithium keypair -> fingerprint
    DerivedDnaIdentity deriveFromWords(const QString& mnemonicWords,
                                      const QString& passphrase);

} // namespace
