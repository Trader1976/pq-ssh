#include "DnaIdentityDerivation.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QHash>
#include <QRandomGenerator>

#include <openssl/evp.h>
//#include <openssl/sha.h>

// -----------------------------------------------------------------------------
// External deterministic ML-DSA-87 (Dilithium5) keypair generator
// -----------------------------------------------------------------------------
extern "C" {
    int qgp_dsa87_keypair_derand(unsigned char* pk,
                                unsigned char* sk,
                                const unsigned char seed[32]);
}

// -----------------------------------------------------------------------------
// secure_memzero / secureZero
// -----------------------------------------------------------------------------
static void secure_memzero(void* p, size_t n)
{
    volatile unsigned char* v = reinterpret_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

static void secureZero(QByteArray& b)
{
    if (!b.isEmpty())
        secure_memzero(b.data(), (size_t)b.size());
}

// -----------------------------------------------------------------------------
// ML-DSA-87 fixed sizes
// -----------------------------------------------------------------------------
static constexpr int DSA87_PK_BYTES = 2592;
static constexpr int DSA87_SK_BYTES = 4896;

namespace {

// Read whole QRC text file lines
static QStringList readWordlistLines(const QString& qrcPath, QString* err)
{
    if (err) err->clear();

    QFile f(qrcPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QString("Cannot open wordlist '%1': %2").arg(qrcPath, f.errorString());
        return {};
    }

    QStringList out;
    out.reserve(2048);

    while (!f.atEnd()) {
        const QString w = QString::fromUtf8(f.readLine()).trimmed();
        if (!w.isEmpty())
            out << w;
    }
    return out;
}

    static QByteArray sha256(const QByteArray& in)
{
    QByteArray out(32, 0);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    const int ok =
        (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) &&
        (EVP_DigestUpdate(ctx, in.constData(), (size_t)in.size()) == 1) &&
        (EVP_DigestFinal_ex(ctx,
            reinterpret_cast<unsigned char*>(out.data()),
            nullptr) == 1);

    EVP_MD_CTX_free(ctx);
    if (!ok) return {};
    return out;
}

// Extract bit i from byte array (MSB-first)
static int getBitMSB(const QByteArray& bytes, int i)
{
    const int byteIndex = i / 8;
    const int bitInByte = 7 - (i % 8);
    const unsigned char b = (unsigned char)bytes[byteIndex];
    return (b >> bitInByte) & 1;
}

// Set bit i in byte array (MSB-first)
static void setBitMSB(QByteArray& bytes, int i, int v)
{
    const int byteIndex = i / 8;
    const int bitInByte = 7 - (i % 8);
    unsigned char b = (unsigned char)bytes[byteIndex];
    if (v) b |= (1u << bitInByte);
    else   b &= ~(1u << bitInByte);
    bytes[byteIndex] = (char)b;
}

static QStringList splitMnemonicWordsNormalized(const QString& mnemonicNfkdLower)
{
    QString s = mnemonicNfkdLower;
    s.replace('\r', ' ');
    s.replace('\n', ' ');
    s = s.simplified();
    if (s.isEmpty()) return {};
    return s.split(' ', Qt::SkipEmptyParts);
}

} // namespace

namespace DnaIdentityDerivation {


QByteArray deriveEd25519Seed32FromMaster(const QByteArray& masterSeed64)
{
    if (masterSeed64.size() != 64)
        return {};

    // Domain separation: must stay stable forever
    const QByteArray ctx = QByteArrayLiteral("cpunk-pqssh-ed25519-v1");

    // SHAKE256(master || ctx) -> 32 bytes
    const QByteArray in = masterSeed64 + ctx;
    return shake256_32(in);
}

// -----------------------------------------------------------------------------
// normalizeMnemonic (BIP39: NFKD + lowercase + single spaces)
// -----------------------------------------------------------------------------
QString normalizeMnemonic(const QString& words)
{
    QString t = words;
    t.replace('\r', ' ');
    t.replace('\n', ' ');
    t = t.simplified();
    t = t.normalized(QString::NormalizationForm_KD);
    return t.toLower();
}

// -----------------------------------------------------------------------------
// BIP39 wordlist loader (QRC)
// -----------------------------------------------------------------------------
QStringList loadBip39EnglishWordlist(QString* err)
{
    QStringList wl = readWordlistLines(":/wordlists/bip39_english.txt", err);
    if (wl.size() != 2048) {
        if (err && err->isEmpty())
            *err = QString("Wordlist size is %1, expected 2048.").arg(wl.size());
    }
    return wl;
}

// -----------------------------------------------------------------------------
// Convert 24-word mnemonic -> 32-byte entropy (validates membership + checksum)
// -----------------------------------------------------------------------------
bool bip39MnemonicToEntropy24(const QString& mnemonicWords,
                              QByteArray* entropyOut32,
                              QString* err)
{
    if (err) err->clear();
    if (entropyOut32) entropyOut32->clear();

    QString wlErr;
    const QStringList wl = loadBip39EnglishWordlist(&wlErr);
    if (wl.size() != 2048) {
        if (err) *err = wlErr.isEmpty() ? QString("Invalid BIP39 wordlist.") : wlErr;
        return false;
    }

    const QStringList words = splitMnemonicWordsNormalized(normalizeMnemonic(mnemonicWords));
    if (words.size() != 24) {
        if (err) *err = QString("Expected 24 words, got %1.").arg(words.size());
        return false;
    }

    // Cache map for fast lookups (per-process)
    static QStringList cachedWl;
    static QHash<QString,int> cachedMap;
    if (cachedWl != wl) {
        cachedWl = wl;
        cachedMap.clear();
        cachedMap.reserve(2048);
        for (int i = 0; i < wl.size(); ++i)
            cachedMap.insert(wl[i], i);
    }

    // 24*11 = 264 bits = 256 entropy + 8 checksum
    QByteArray bitsBytes(33, 0);
    int bitPos = 0;

    for (const QString& w : words) {
        auto it = cachedMap.find(w);
        if (it == cachedMap.end()) {
            if (err) *err = QString("Word not in BIP39 list: '%1'").arg(w);
            return false;
        }
        const int idx = it.value(); // 0..2047

        for (int b = 10; b >= 0; --b) {
            const int v = (idx >> b) & 1;
            setBitMSB(bitsBytes, bitPos++, v);
        }
    }

    QByteArray entropy(32, 0);
    for (int i = 0; i < 256; ++i)
        setBitMSB(entropy, i, getBitMSB(bitsBytes, i));

    int csGiven = 0;
    for (int i = 0; i < 8; ++i)
        csGiven = (csGiven << 1) | getBitMSB(bitsBytes, 256 + i);

    const QByteArray h = sha256(entropy);
    const int csExpected = (unsigned char)h[0]; // first 8 bits

    if (csGiven != csExpected) {
        if (err) *err = QString("BIP39 checksum mismatch.");
        return false;
    }

    if (entropyOut32) *entropyOut32 = entropy;
    return true;
}

bool validateBip39Mnemonic24(const QString& mnemonicWords, QString* err)
{
    QByteArray entropy;
    return bip39MnemonicToEntropy24(mnemonicWords, &entropy, err);
}

// -----------------------------------------------------------------------------
// Generate real 24-word BIP39 mnemonic (256-bit entropy + checksum)
// -----------------------------------------------------------------------------
QString generateBip39Mnemonic24(QString* err)
{
    if (err) err->clear();

    QString wlErr;
    const QStringList wl = loadBip39EnglishWordlist(&wlErr);
    if (wl.size() != 2048) {
        if (err) *err = wlErr.isEmpty() ? QString("Invalid BIP39 wordlist.") : wlErr;
        return {};
    }

    // 256-bit entropy
    QByteArray entropy(32, 0);

    // QRandomGenerator::generate takes (begin, end) pointers (Qt5)
    auto *begin = reinterpret_cast<quint32*>(entropy.data());
    auto *end   = begin + (entropy.size() / 4); // 32 bytes -> 8 quint32
    QRandomGenerator::system()->generate(begin, end);

    // checksum = first 8 bits of SHA256(entropy)
    const QByteArray h = sha256(entropy);
    const unsigned char cs = (unsigned char)h[0];

    // 264 bits total
    QByteArray bitsBytes(33, 0);
    for (int i = 0; i < 256; ++i)
        setBitMSB(bitsBytes, i, getBitMSB(entropy, i));

    for (int i = 0; i < 8; ++i) {
        const int v = (cs >> (7 - i)) & 1;
        setBitMSB(bitsBytes, 256 + i, v);
    }

    QStringList out;
    out.reserve(24);

    int bitPos = 0;
    for (int w = 0; w < 24; ++w) {
        int idx = 0;
        for (int i = 0; i < 11; ++i)
            idx = (idx << 1) | getBitMSB(bitsBytes, bitPos++);
        out << wl[idx];
    }

    // BIP39 uses spaces between words
    return out.join(' ');
}

// -----------------------------------------------------------------------------
// bip39Seed64 (PBKDF2-HMAC-SHA512 per BIP39)
// -----------------------------------------------------------------------------
QByteArray bip39Seed64(const QString& mnemonicWords,
                       const QString& passphrase)
{
    QByteArray out(64, 0);

    // BIP39 expects NFKD for both mnemonic and passphrase.
    const QString mn = normalizeMnemonic(mnemonicWords);
    const QString pp = QString(passphrase).normalized(QString::NormalizationForm_KD);

    const QByteArray m = mn.toUtf8();
    const QByteArray salt = QByteArray("mnemonic") + pp.toUtf8();

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

// -----------------------------------------------------------------------------
// SHAKE256(input) -> 32 bytes
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// SHA3-512 hex (128 chars)
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// deriveFromWords
// -----------------------------------------------------------------------------
DerivedDnaIdentity deriveFromWords(const QString& mnemonicWords,
                                  const QString& passphrase)
{
    DerivedDnaIdentity r;

    // Validate mnemonic (24 words, in list, checksum ok)
    {
        QString vErr;
        if (!validateBip39Mnemonic24(mnemonicWords, &vErr)) {
            r.error = vErr.isEmpty() ? QString("Invalid BIP39 mnemonic.") : vErr;
            return r;
        }
    }

    // 1) BIP39 master seed (64)
    QByteArray master = bip39Seed64(mnemonicWords, passphrase);
    if (master.size() != 64) {
        r.error = "PBKDF2 seed derivation failed";
        return r;
    }

    // Optionally expose to caller
    r.masterSeed64 = master;

    // 2) Domain-separated deterministic 32-byte seed for ML-DSA-87
    const QByteArray ctx = QByteArrayLiteral("qgp-signing-v1"); // must stay stable forever
    QByteArray in = master + ctx;

    QByteArray seed32 = shake256_32(in);
    if (seed32.size() != 32) {
        r.error = "SHAKE256 seed derivation failed";
        secureZero(master);
        secureZero(in);
        return r;
    }

    r.signingSeed32 = seed32;

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
        secureZero(r.masterSeed64);
        secureZero(r.signingSeed32);
        secureZero(r.dilithiumPk);
        secureZero(r.dilithiumSk);

        r.masterSeed64.clear();
        r.signingSeed32.clear();
        r.dilithiumPk.clear();
        r.dilithiumSk.clear();
        return r;
    }

    // 4) Fingerprint = SHA3-512(pk) hex
    r.fingerprintHex128 = sha3_512_hex(r.dilithiumPk);
    r.ok = true;

    // Wipe temporaries (caller can wipe r.masterSeed64 / r.signingSeed32 if not needed)
    secureZero(master);
    secureZero(in);
    secureZero(seed32);

    return r;
}

} // namespace DnaIdentityDerivation
