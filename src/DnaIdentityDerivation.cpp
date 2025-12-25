#include "DnaIdentityDerivation.h"

#include <QRegularExpression>
#include <openssl/evp.h>

extern "C" {
    int qgp_dsa87_keypair_derand(unsigned char* pk, unsigned char* sk, const unsigned char seed[32]);
}

static void secure_memzero(void* p, size_t n) {
    volatile unsigned char* v = reinterpret_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

static void secureZero(QByteArray& b) {
    if (!b.isEmpty()) secure_memzero(b.data(), (size_t)b.size());
}

// If you have constants in headers, use those instead of literals.
static constexpr int DSA87_PK_BYTES = 2592;
static constexpr int DSA87_SK_BYTES = 4896;

namespace DnaIdentityDerivation {

QString normalizeMnemonic(const QString& words)
{
    QString t = words.trimmed();
    t.replace(QRegularExpression("\\s+"), " ");
    return t.toLower();
}

QByteArray bip39Seed64(const QString& mnemonicWords,
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

QByteArray shake256_32(const QByteArray& input)
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

QString sha3_512_hex(const QByteArray& bytes)
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

// ✅ THIS MUST EXIST, and must match header exactly:
DerivedDnaIdentity deriveFromWords(const QString& mnemonicWords,
                                  const QString& passphrase)
{
    DerivedDnaIdentity r;

    const QString normalized = normalizeMnemonic(mnemonicWords);

    // 1) BIP39 master seed (64)
    QByteArray master = bip39Seed64(normalized, passphrase);
    if (master.size() != 64) {
        r.error = "PBKDF2 seed derivation failed";
        return r;
    }

    // 2) Deterministic 32-byte seed for ML-DSA-87 (match your DNA contexts!)
    const QByteArray ctx = QByteArrayLiteral("qgp-signing-v1"); // <-- replace if you used different
    QByteArray in = master + ctx;
    QByteArray seed32 = shake256_32(in);
    if (seed32.size() != 32) {
        r.error = "SHAKE256 seed derivation failed";
        secureZero(master);
        secureZero(in);
        return r;
    }

    // 3) Deterministic Dilithium keypair
    r.dilithiumPk = QByteArray(DSA87_PK_BYTES, 0);
    r.dilithiumSk = QByteArray(DSA87_SK_BYTES, 0);

    if (qgp_dsa87_keypair_derand(
            reinterpret_cast<unsigned char*>(r.dilithiumPk.data()),
            reinterpret_cast<unsigned char*>(r.dilithiumSk.data()),
            reinterpret_cast<const unsigned char*>(seed32.constData())) != 0) {
        r.error = "qgp_dsa87_keypair_derand failed";
        secureZero(master);
        secureZero(in);
        secureZero(seed32);
        secureZero(r.dilithiumPk);
        secureZero(r.dilithiumSk);
        r.dilithiumPk.clear();
        r.dilithiumSk.clear();
        return r;
    }

    // 4) Fingerprint = SHA3-512(pk) hex
    r.fingerprintHex128 = sha3_512_hex(r.dilithiumPk);
    r.ok = true;

    // Wipe temporary secrets (keep pk/sk only if you truly need them)
    secureZero(master);
    secureZero(in);
    secureZero(seed32);

    return r;
}

} // namespace DnaIdentityDerivation
