// DilithiumKeyCrypto.cpp
#include "DilithiumKeyCrypto.h"
#include <sodium.h>

static constexpr char MAGIC[] = "PQSSH1"; // 6 bytes, no NUL stored

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

    if (!sodiumInitOnce(err)) return false;

    if (passphrase.isEmpty()) {
        if (err) *err = "Empty passphrase";
        return false;
    }

    const QByteArray passUtf8 = passphrase.toUtf8();

    unsigned char salt[crypto_pwhash_SALTBYTES];
    randombytes_buf(salt, sizeof salt);

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

    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    randombytes_buf(nonce, sizeof nonce);

    QByteArray cipher;
    cipher.resize(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);

    unsigned long long clen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(cipher.data()), &clen,
        reinterpret_cast<const unsigned char*>(plain.constData()),
        (unsigned long long)plain.size(),
        nullptr, 0, nullptr,
        nonce, key
    );

    if (rc != 0) {
        if (err) *err = "XChaCha20-Poly1305 encrypt failed";
        sodium_memzero(key, sizeof key);
        return false;
    }

    cipher.resize((int)clen);

    outEncrypted->clear();
    outEncrypted->append(MAGIC, 6);
    outEncrypted->append(reinterpret_cast<const char*>(salt), (int)sizeof salt);
    outEncrypted->append(reinterpret_cast<const char*>(nonce), (int)sizeof nonce);
    outEncrypted->append(cipher);

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

    const int magicLen = 6;
    const int saltLen  = crypto_pwhash_SALTBYTES;
    const int nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const int headerLen = magicLen + saltLen + nonceLen;

    if (encrypted.size() < headerLen + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (err) *err = "Encrypted blob too small";
        return false;
    }

    if (encrypted.left(magicLen) != QByteArray(MAGIC, magicLen)) {
        if (err) *err = "Bad magic (not PQSSH1)";
        return false;
    }

    const unsigned char* salt  = reinterpret_cast<const unsigned char*>(encrypted.constData() + magicLen);
    const unsigned char* nonce = reinterpret_cast<const unsigned char*>(encrypted.constData() + magicLen + saltLen);

    const QByteArray cipher = encrypted.mid(headerLen);

    const QByteArray passUtf8 = passphrase.toUtf8();

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

    if (cipher.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (err) *err = "Ciphertext too small";
        sodium_memzero(key, sizeof key);
        return false;
    }

    QByteArray plain;
    plain.resize(cipher.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);

    unsigned long long plen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plain.data()), &plen,
        nullptr,
        reinterpret_cast<const unsigned char*>(cipher.constData()),
        (unsigned long long)cipher.size(),
        nullptr, 0,
        nonce, key
    );

    sodium_memzero(key, sizeof key);

    if (rc != 0) {
        if (err) *err = "Decrypt failed (wrong passphrase or corrupted data)";
        return false;
    }

    plain.resize((int)plen);
    *outPlain = plain;
    return true;
}
