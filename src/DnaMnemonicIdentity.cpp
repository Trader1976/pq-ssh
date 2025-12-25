#include "DnaMnemonicIdentity.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
extern "C" {
#include "dna_vendor/crypto/dsa/utils/qgp_dilithium.h"   // should define QGP_DSA87_PUBLICKEYBYTES / SECRETKEYBYTES
}
#include <QStringList>

// ---- C functions from vendored DNA/QGP code ----
extern "C" {
    // from your copied qgp_dilithium.c
    int qgp_dsa87_keypair_derand(unsigned char *pk, unsigned char *sk, const unsigned char *seed);

    // from Dilithium ref
    // CRYPTO_PUBLICKEYBYTES / CRYPTO_SECRETKEYBYTES are typically defined in api.h
    #include "dna_vendor/crypto/dsa/api.h"
    #include "dna_vendor/crypto/dsa/fips202.h"
}

// BIP39 seed derivation: PBKDF2-HMAC-SHA512(password=mnemonic, salt="mnemonic"+passphrase, iter=2048, dkLen=64)
static QByteArray bip39Seed64(const QString &mnemonicWords, const QString &passphrase)
{
    // IMPORTANT: BIP39 specifies NFKD normalization. DNA code you showed does not.
    // If you want maximal compatibility, add Unicode normalization policy here.
    const QByteArray mnemonic = mnemonicWords.simplified().toUtf8();
    const QByteArray salt = QByteArray("mnemonic") + passphrase.simplified().toUtf8();

    QByteArray out(64, 0);
    const int ok = PKCS5_PBKDF2_HMAC(
        mnemonic.constData(), mnemonic.size(),
        reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(),
        2048,
        EVP_sha512(),
        out.size(),
        reinterpret_cast<unsigned char*>(out.data())
    );

    if (ok != 1) return {};
    return out;
}

// SHAKE256(master_seed || context, 32)
static QByteArray shake256_32(const QByteArray &masterSeed64, const QByteArray &ctx)
{
    QByteArray input = masterSeed64 + ctx;
    QByteArray out(32, 0);
    shake256(reinterpret_cast<unsigned char*>(out.data()), 32,
             reinterpret_cast<const unsigned char*>(input.constData()), input.size());
    return out;
}

static QString sha3_512_hex(const QByteArray &bytes)
{
    unsigned char md[64];
    unsigned int mdLen = 0;

    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c) return {};
    if (EVP_DigestInit_ex(c, EVP_sha3_512(), nullptr) != 1) { EVP_MD_CTX_free(c); return {}; }
    if (EVP_DigestUpdate(c, bytes.constData(), bytes.size()) != 1) { EVP_MD_CTX_free(c); return {}; }
    if (EVP_DigestFinal_ex(c, md, &mdLen) != 1) { EVP_MD_CTX_free(c); return {}; }
    EVP_MD_CTX_free(c);

    if (mdLen != 64) return {};

    QString hex;
    hex.reserve(128);
    for (int i = 0; i < 64; ++i) {
        hex += QString("%1").arg(md[i], 2, 16, QLatin1Char('0'));
    }
    return hex;
}

DnaIdentityKeys DnaMnemonicIdentity::deriveDsa87From24Words(const QString &words24,
                                                           const QString &passphrase)
{
    DnaIdentityKeys out;

    // Optional UX check: enforce 24 tokens (DNA uses BIP39 validation; you can later add full validation)
    const QStringList toks = words24.simplified().split(' ', Qt::SkipEmptyParts);
    if (toks.size() != 24) {
        return out;
    }

    // 1) master_seed[64] using BIP39 PBKDF2
    const QByteArray master = bip39Seed64(words24, passphrase);
    if (master.size() != 64) return out;

    // 2) signing_seed[32] = SHAKE256(master || "qgp-signing-v1", 32)
    const QByteArray signingSeed = shake256_32(master, QByteArray("qgp-signing-v1"));
    if (signingSeed.size() != 32) return out;

    // 3) Dilithium keypair from seed (ML-DSA-87)
    QByteArray pk(QGP_DSA87_PUBLICKEYBYTES, 0);
    QByteArray sk(QGP_DSA87_SECRETKEYBYTES, 0);

    if (qgp_dsa87_keypair_derand(
            reinterpret_cast<unsigned char*>(pk.data()),
            reinterpret_cast<unsigned char*>(sk.data()),
            reinterpret_cast<const unsigned char*>(signingSeed.constData())) != 0) {
        return {};
    }

    // 4) fingerprint = SHA3-512(pk) hex
    out.fingerprintHex = sha3_512_hex(pk);
    out.dsaPk = pk;
    out.dsaSk = sk;
    return out;
}
