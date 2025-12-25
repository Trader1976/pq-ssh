#include "DnaIdentityDerivation.h"

#include <QRegularExpression>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

// Bring in the SAME function pq-ssh will link from QGP/DNA crypto.
// Adjust include / symbol name if needed.
extern "C" {
    int qgp_dsa87_keypair_derand(unsigned char* pk, unsigned char* sk, const unsigned char seed[32]);
}

// If you have constants in headers, use those instead of literals.
static constexpr int DSA87_PK_BYTES = 2592;
// Secret size: in Dilithium5 it is commonly 4896; use your QGP constant if available.
static constexpr int DSA87_SK_BYTES = 4896;

static void secure_memzero(void* p, size_t n) {
    volatile unsigned char* v = reinterpret_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

static void secureZero(QByteArray& b) {
    if (!b.isEmpty()) secure_memzero(b.data(), (size_t)b.size());
}

QString DnaIdentityDerivation::normalizeMnemonic(const QString& words)
{
    QString t = words.trimmed();
    t.replace(QRegularExpression("\\s+"), " ");
    return t.toLower();
}

QByteArray DnaIdentityDerivation::bip39Seed64(const QString& mnemonicWords,
                                             const QString& passphrase)
{
    QByteArray out(64, 0);

    const QByteArray m = normalizeMnemonic(mnemonicWords).toUtf8();
    const QByteArray salt = QByteArray("mnemonic") + passphrase.simplified().toUtf8();

    if (!PKCS5_PBKDF2_HMAC(
            m.constData(), m.size(),
            reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(),
            2048,
            EVP_sha512(),
            out.size(),
            reinterpret_cast<unsigned char*>(out.data())))
        return {};

    return out;
}

QByteArray DnaIdentityDerivation::shake256_32(const QByteArray& input)
{
    QByteArray out(32, 0);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    const EVP_MD* md = EVP_shake256();
    if (!md) { EVP_MD_CTX_free(ctx); return {}; }

    const int ok =
        (EVP_DigestInit_ex(ctx, md, nullptr) == 1) &&
        (EVP_DigestUpdate(ctx, input.constData(), input.size()) == 1) &&
        (EVP_DigestFinalXOF(ctx,
            reinterpret_cast<unsigned char*>(out.data()),
            out.size()) == 1);

    EVP_MD_CTX_free(ctx);

    if (!ok) return {};
    return out;
}

QString DnaIdentityDerivation::sha3_512_hex(const QByteArray& bytes)
{
    unsigned char out[64];
    unsigned int len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    EVP_DigestInit_ex(ctx, EVP_sha3_512(), nullptr);
    EVP_DigestUpdate(ctx, bytes.constData(), bytes.size());
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);

    QString h;
    h.reserve(128);
    for (int i = 0; i < 64; ++i)
        h += QString("%1").arg(out[i], 2, 16, QLatin1Char('0'));
    return h;
}

DerivedDnaIdentity DnaIdentityDerivation::deriveFromWords(const QString& mnemonicWords,
                                                         const QString& passphrase)
{
    DerivedDnaIdentity r;

    // 1) master seed (BIP39-style, like your IdentityDerivation does already)
    QByteArray master = bip39Seed64(mnemonicWords, passphrase);
    if (master.size() != 64) { r.error = "PBKDF2 seed derivation failed"; return r; }

    // 2) SHAKE-based derivation (this is the key difference vs pq-ssh HKDF)
    const QByteArray signingCtx("qgp-signing-v1");
    const QByteArray encCtx("qgp-encryption-v1");

    QByteArray signingSeed = shake256_32(master + signingCtx);
    QByteArray encSeed = shake256_32(master + encCtx);

    if (signingSeed.size() != 32 || encSeed.size() != 32) {
        r.error = "SHAKE256 seed derivation failed";
        secureZero(master);
        return r;
    }

    // 3) Deterministic Dilithium keypair from signing seed (DNA fingerprint basis)
    QByteArray pk(DSA87_PK_BYTES, 0);
    QByteArray sk(DSA87_SK_BYTES, 0);

    if (qgp_dsa87_keypair_derand(
            reinterpret_cast<unsigned char*>(pk.data()),
            reinterpret_cast<unsigned char*>(sk.data()),
            reinterpret_cast<const unsigned char*>(signingSeed.constData())) != 0) {
        r.error = "qgp_dsa87_keypair_derand failed";
        secureZero(master);
        secureZero(signingSeed);
        secureZero(encSeed);
        secureZero(sk);
        return r;
    }

    r.masterSeed64 = master;      // you can wipe immediately if you don’t want to keep it
    r.signingSeed32 = signingSeed;
    r.encryptionSeed32 = encSeed;
    r.dilithiumPk = pk;
    r.dilithiumSk = sk;
    r.fingerprintHex128 = sha3_512_hex(pk);
    r.ok = true;

    // Optional: wipe master immediately (DNA does)
    // If you need it for wallet derivation later, don’t wipe until you’ve stored it encrypted.
    secureZero(r.masterSeed64);

    return r;
}
