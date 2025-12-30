#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

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

// Derive a deterministic 32-byte Ed25519 seed from an already-derived BIP39 master seed (64 bytes).
// Domain-separated and stable forever.
QByteArray deriveEd25519Seed32FromMaster(const QByteArray& masterSeed64);

// True BIP39 wants NFKD normalization. We'll do it.
QString normalizeMnemonic(const QString& words);

// PBKDF2-HMAC-SHA512(password=mnemonic, salt="mnemonic"+passphrase, iter=2048, out=64)
QByteArray bip39Seed64(const QString& mnemonicWords,
                       const QString& passphrase);

// Alias (optional; keeps old callsites compiling)
inline QByteArray bip39MasterSeed64(const QString& mnemonicWords,
                                    const QString& passphrase)
{
    return bip39Seed64(mnemonicWords, passphrase);
}

// === NEW: Real BIP39 helpers (English list from QRC) ===

// Loads 2048 words from ":/wordlists/bip39_english.txt"
QStringList loadBip39EnglishWordlist(QString* err = nullptr);

// 24-word mnemonic generation (256-bit entropy + checksum)
QString generateBip39Mnemonic24(QString* err = nullptr);

// Validates a 24-word mnemonic against the wordlist and checksum.
// Returns true if valid; otherwise returns false and sets err.
bool validateBip39Mnemonic24(const QString& mnemonicWords, QString* err = nullptr);

// (Optional) Extract entropy back (32 bytes) if you want it.
bool bip39MnemonicToEntropy24(const QString& mnemonicWords,
                              QByteArray* entropyOut32,
                              QString* err = nullptr);

// DNA seed derivation helper: SHAKE256(input, 32)
QByteArray shake256_32(const QByteArray& input);

// SHA3-512 hex (128 chars)
QString sha3_512_hex(const QByteArray& bytes);

// End-to-end: mnemonic -> seeds -> Dilithium keypair -> fingerprint
DerivedDnaIdentity deriveFromWords(const QString& mnemonicWords,
                                  const QString& passphrase);

} // namespace DnaIdentityDerivation
