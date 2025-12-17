// DilithiumKeyCrypto.cpp
//
// PURPOSE
// -------
// This module provides *local* encryption/decryption for Dilithium private key material
// (or any PQ private key blob) before it is stored on disk.
//
// It deliberately does NOT generate keys and does NOT talk to SSH servers.
// It is a small, auditable "crypto utility" with a stable on-disk format.
//
// SECURITY MODEL (high level)
// ---------------------------
// - User supplies a passphrase.
// - We derive a symmetric key using Argon2id (libsodium crypto_pwhash) with a random salt.
// - We encrypt using XChaCha20-Poly1305 (AEAD: confidentiality + integrity).
// - The output is a single binary blob with a small header.
//
// FILE/BLOB FORMAT (current)
// --------------------------
// [ MAGIC (6 bytes) | SALT (16 bytes) | NONCE (24 bytes) | CIPHERTEXT+TAG (N bytes) ]
//
// MAGIC  : ASCII "PQSSH1"  (6 bytes; no NUL terminator stored)
// SALT   : crypto_pwhash_SALTBYTES (currently 16)
// NONCE  : crypto_aead_xchacha20poly1305_ietf_NPUBBYTES (currently 24)
// TAG    : crypto_aead_xchacha20poly1305_ietf_ABYTES (currently 16), included in ciphertext output
//
// FORMAT EVOLUTION / GOTCHAS
// --------------------------
// 1) Versioning:
//    - MAGIC doubles as a version marker. If the layout changes, bump MAGIC (e.g. "PQSSH2").
//    - If you bump MAGIC, keep PQSSH1 decrypt support (migration) in a higher-level module.
//
// 2) KDF parameters:
//    - Today we hardcode crypto_pwhash_OPSLIMIT_MODERATE / MEMLIMIT_MODERATE.
//    - If you ever change these, old blobs become undecryptable unless the parameters are stored.
//    - Recommended future header extension: store opsLimit+memLimit (and alg) in the header.
//
// 3) Associated Data (AD):
//    - We currently pass AD=nullptr. AEAD still authenticates ciphertext, but header is not
//      *explicitly* bound as AD. It is indirectly validated (wrong header => wrong key/nonce => fail).
//    - Recommended future: use AD = header bytes (MAGIC+salt+nonce+params) to bind the header.
//
// 4) Secret handling:
//    - Derived key is wiped with sodium_memzero().
//    - Plaintext is returned to caller; caller MUST wipe it after use.
//
// 5) Randomness:
//    - salt + nonce come from randombytes_buf() (libsodium CSPRNG). No manual RNG.
//
// 6) Side-channels / logging:
//    - Never log passphrase, plaintext, derived key, or ciphertext.
//    - Logging sizes + filenames is OK.
//
// Notes:
// - The salt is required to derive the same key during decryption.
// - XChaCha20 uses a 24-byte nonce which is safe to generate randomly.
// - We do NOT include "associated data" (AD) at the moment; header integrity is validated
//   indirectly because the wrong salt/nonce produces wrong key/nonce and AEAD verify fails.
//
// IMPORTANT: Do not log plaintext, passphrases, or derived keys.
// Only log sizes, file names, and non-sensitive diagnostics.

#include "DilithiumKeyCrypto.h"
#include <sodium.h>

static constexpr char MAGIC[] = "PQSSH1"; // 6 bytes, no NUL stored

// libsodium requires sodium_init() once per process.
// This helper makes encryption/decryption safe to call from anywhere without worrying about init order.
static bool sodiumInitOnce(QString* err)
{
    static bool inited = false;
    if (inited) return true;

    if (sodium_init() < 0) {
        if (err) *err = "libsodium init failed";
        return false;
    }
    inited = true;
    return true;
}

bool encryptDilithiumKey(
    const QByteArray& plain,
    const QString& passphrase,
    QByteArray* outEncrypted,
    QString* err
)
{
    if (err) err->clear();
    if (!outEncrypted) return false;

    // Ensure libsodium is ready before using any crypto APIs.
    if (!sodiumInitOnce(err)) return false;

    // A passphrase is mandatory. (No silent "empty passphrase" mode.)
    if (passphrase.isEmpty()) {
        if (err) *err = "Empty passphrase";
        return false;
    }

    // Convert passphrase to UTF-8 bytes for KDF input.
    // NOTE: UTF-8 makes passphrases portable across platforms/locales.
    const QByteArray passUtf8 = passphrase.toUtf8();

    // Random salt: makes identical passphrases produce different derived keys.
    // Stored in the blob header.
    unsigned char salt[crypto_pwhash_SALTBYTES];
    randombytes_buf(salt, sizeof salt);

    // Build header bytes to authenticate as Associated Data (AD).
    // This binds MAGIC + salt + nonce to the AEAD tag.
    QByteArray header;
    header.append(MAGIC, 6);
    header.append(reinterpret_cast<const char*>(salt), (int)sizeof salt);

    // Derive an AEAD key from the passphrase using Argon2id.
    // MODERATE limits: reasonable interactive security vs performance tradeoff.
    unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    if (crypto_pwhash(
            key, sizeof key,
            passUtf8.constData(),
            (unsigned long long)passUtf8.size(),
            salt,
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE,
            crypto_pwhash_ALG_ARGON2ID13
        ) != 0)
    {
        if (err) *err = "Argon2id failed (crypto_pwhash)";
        return false;
    }

    // XChaCha20-Poly1305 uses a 24-byte nonce.
    // Random nonce is safe for XChaCha (unlike classic ChaCha20 with 12-byte nonces).
    // Stored in the blob header.
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(nonce, sizeof nonce);

    // Now that nonce exists, append it into header (and thus into AD)
    header.append(reinterpret_cast<const char*>(nonce), (int)sizeof nonce);

    // Allocate ciphertext buffer: plaintext + authentication tag.
    QByteArray cipher;
    cipher.resize(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);

    unsigned long long clen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(cipher.data()), &clen,
        reinterpret_cast<const unsigned char*>(plain.constData()),
        (unsigned long long)plain.size(),
        reinterpret_cast<const unsigned char*>(header.constData()),
        (unsigned long long)header.size(),
        nullptr,
        nonce, key
    );

    if (rc != 0) {
        if (err) *err = "XChaCha20-Poly1305 encrypt failed";
        // Wipe derived key before returning.
        sodium_memzero(key, sizeof key);
        return false;
    }

    cipher.resize((int)clen);

    // Build the final blob: header + ciphertext
    outEncrypted->clear();
    outEncrypted->append(header);
    outEncrypted->append(cipher);

    // Wipe derived key from stack memory.
    sodium_memzero(key, sizeof key);
    return true;
}

bool decryptDilithiumKey(
    const QByteArray& encrypted,
    const QString& passphrase,
    QByteArray* outPlain,
    QString* err
)
{
    if (err) err->clear();
    if (!outPlain) return false;

    if (!sodiumInitOnce(err)) return false;

    if (passphrase.isEmpty()) {
        if (err) *err = "Empty passphrase";
        return false;
    }

    // Header sizes (kept explicit for readability + format stability).
    const int magicLen = 6;
    const int saltLen  = crypto_pwhash_SALTBYTES;
    const int nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const int headerLen = magicLen + saltLen + nonceLen;

    // Must include at least header + AEAD tag.
    if (encrypted.size() < headerLen + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (err) *err = "Encrypted blob too small";
        return false;
    }

    // Format/version guard.
    // If you ever change format, bump MAGIC (e.g., PQSSH2) and add migration logic elsewhere.
    if (encrypted.left(magicLen) != QByteArray(MAGIC, magicLen)) {
        if (err) *err = "Bad magic (not PQSSH1)";
        return false;
    }

    // Salt and nonce are taken directly from the header.
    // These pointers reference encrypted's internal storage (valid during this function).
    const unsigned char* salt  = reinterpret_cast<const unsigned char*>(encrypted.constData() + magicLen);
    const unsigned char* nonce = reinterpret_cast<const unsigned char*>(encrypted.constData() + magicLen + saltLen);

    // Ciphertext includes the Poly1305 authentication tag at the end (libsodium convention).
    const QByteArray cipher = encrypted.mid(headerLen);

    // Rebuild header bytes for Associated Data (must match encrypt side)
    const QByteArray header = encrypted.left(headerLen);

    const QByteArray passUtf8 = passphrase.toUtf8();

    // Re-derive the same AEAD key from passphrase+salt.
    unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    if (crypto_pwhash(
            key, sizeof key,
            passUtf8.constData(),
            (unsigned long long)passUtf8.size(),
            salt,
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE,
            crypto_pwhash_ALG_ARGON2ID13
        ) != 0)
    {
        if (err) *err = "Argon2id failed (crypto_pwhash)";
        return false;
    }

    // Must be at least a tag.
    if (cipher.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (err) *err = "Ciphertext too small";
        sodium_memzero(key, sizeof key);
        return false;
    }

    // Allocate worst-case plaintext size (ciphertext minus tag).
    QByteArray plain;
    plain.resize(cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);

    unsigned long long plen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plain.data()), &plen,
        nullptr, // nsec out (unused)
        reinterpret_cast<const unsigned char*>(cipher.constData()),
        (unsigned long long)cipher.size(),
        reinterpret_cast<const unsigned char*>(header.constData()),
        (unsigned long long)header.size(),
        nonce, key
    );

    // Wipe derived key regardless of success.
    sodium_memzero(key, sizeof key);

    if (rc != 0) {
        // AEAD verify failed: wrong passphrase OR corrupted blob (or wrong salt/nonce).
        if (err) *err = "Decrypt failed (wrong passphrase or corrupted data)";
        return false;
    }

    // Trim to actual plaintext length.
    plain.resize((int)plen);
    *outPlain = plain;
    return true;
}
