// IdentityDerivation.cpp
//
// PURPOSE
// -------
// Deterministic identity/key derivation for pq-ssh, based on a mnemonic phrase
// (24 words) + optional passphrase, producing a stable Ed25519 keypair.
//
// This module is intended to support “identity recovery” style workflows:
// - User enters the same words+passphrase again
// - pq-ssh deterministically re-derives the same keypair
//
// ARCHITECTURAL ROLE
// ------------------
// IdentityDerivation sits at a clear boundary:
// - Input: human text (mnemonic words + passphrase) + an app-controlled context string.
// - Output: raw key material suitable for higher-level formatting modules.
// - It does NOT write files, does NOT touch UI, does NOT interact with SSH servers.
//
// The output is consumed by modules like OpenSshEd25519Key (serialization) and
// by UI components like IdentityManagerDialog (orchestration).
//
// CRYPTOGRAPHIC PIPELINE (HIGH LEVEL)
// -----------------------------------
// 1) Normalize inputs (trim/reduce whitespace) -> bytes
// 2) BIP39-like seed derivation using PBKDF2-HMAC-SHA512:
//      seed64 = PBKDF2(mnemonic, salt="mnemonic"+passphrase, iter=2048, dkLen=64)
//    NOTE: This mirrors the BIP39 seed function (iteration count and SHA-512).
//    However, we do NOT validate the mnemonic word list here (language/dictionary/checksum).
//
// 3) Domain separation / key purpose binding using HKDF-like extract+expand:
//      prk = HMAC-SHA512(salt=0^64, ikm=seed64)
//      okm = HMAC-SHA512(key=prk, msg=info || 0x01)
//      seed32 = okm[0..31]
//
//    The infoString is crucial: it ensures the same words+passphrase can safely derive
//    different keys for different purposes/versions without collision.
//
// 4) Ed25519 keypair generation from seed32 using OpenSSL’s raw Ed25519 API:
//      privKey = seed32
//      pubKey  = Ed25519PublicFromPrivate(seed32)
//      priv64  = seed32 || pub32   (64 bytes, OpenSSH/libs commonly use this layout)
//
// SECURITY NOTES
// --------------
// - This module handles highly sensitive material: mnemonic, passphrase, and derived seed.
// - QString/QByteArray are not "securely zeroed"; secrets may remain in memory after use.
//   Consider explicit clearing for hardened builds.
// - Deterministic derivation means: compromise of mnemonic+passphrase compromises all derived keys.
// - Domain separation string must be stable and versioned; changing it changes derived identities.
//
// COMPATIBILITY / EXPECTATIONS
// ----------------------------
// - This implementation does not enforce "24 words", dictionary, or checksum.
//   It is compatible with *BIP39 seed derivation*, but not full BIP39 mnemonic validation.
// - Normalization uses QString::simplified(), which:
//     - trims ends
//     - collapses internal whitespace to single spaces
//   It does NOT normalize Unicode beyond what Qt does internally.
//   If you expect non-ASCII words, consider explicit Unicode normalization (NFKD/NFC) policy.
//
// IMPLEMENTATION NOTES
// --------------------
// - PKCS5_PBKDF2_HMAC is used for PBKDF2. (OpenSSL)
// - HMAC(EVP_sha512) is used for HKDF steps. (OpenSSL)
// - HKDF implementation here is minimal: extract with zero salt, expand for a single block.
//   That’s sufficient because we only need 32 bytes and SHA-512 output is 64 bytes.

#include "IdentityDerivation.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

static QByteArray norm(const QString &s)
{
    // Normalization policy for user-provided textual secrets.
    //
    // We intentionally keep it simple and deterministic:
    // - simplified(): trims and collapses whitespace to a single space
    // - UTF-8 encoding
    //
    // NOTE: BIP39 specifies NFKD normalization. If you want true BIP39 compatibility
    // across all languages and edge cases, implement explicit Unicode normalization.
    return s.simplified().toUtf8();
}

QByteArray IdentityDerivation::bip39Seed64(const QString &mnemonicWords,
                                          const QString &passphrase)
{
    // BIP39-style seed derivation:
    //   PBKDF2-HMAC-SHA512(password=mnemonic, salt="mnemonic"+passphrase, iter=2048, dkLen=64)
    //
    // IMPORTANT: We do not validate the mnemonic checksum/wordlist here. We treat the
    // mnemonic as an arbitrary user-provided string that should deterministically map
    // to a seed.

    QByteArray out(64, 0);

    const QByteArray m = norm(mnemonicWords);
    const QByteArray salt = QByteArray("mnemonic") + norm(passphrase);

    // OpenSSL returns 1 on success.
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

QByteArray IdentityDerivation::hkdfSha512_32(const QByteArray &ikm,
                                            const QByteArray &info)
{
    // Minimal HKDF-SHA512 implementation tailored for:
    // - output length = 32 bytes
    // - one expand block only (since SHA-512 output is 64 bytes)
    //
    // HKDF Extract:
    //   PRK = HMAC(salt, IKM)
    // Here we use salt = 0^64 (a common convention when no salt is provided).
    //
    // HKDF Expand (first block only):
    //   T1 = HMAC(PRK, info || 0x01)
    //   OKM = T1
    // Return OKM[0..31]

    unsigned char prk[64];
    unsigned int prkLen = 0;
    unsigned char zeroSalt[64] = {0};

    // Extract
    HMAC(EVP_sha512(),
         zeroSalt, sizeof(zeroSalt),
         reinterpret_cast<const unsigned char*>(ikm.constData()), ikm.size(),
         prk, &prkLen);

    if (prkLen != 64) return {};

    // Expand (single block)
    QByteArray msg = info;
    msg.append(char(0x01)); // HKDF counter for the first block

    unsigned char out[64];
    unsigned int outLen = 0;

    HMAC(EVP_sha512(),
         prk, prkLen,
         reinterpret_cast<const unsigned char*>(msg.constData()), msg.size(),
         out, &outLen);

    if (outLen < 32) return {};
    return QByteArray(reinterpret_cast<const char*>(out), 32);
}

DerivedEd25519 IdentityDerivation::deriveEd25519FromWords(
        const QString &mnemonicWords,
        const QString &passphrase,
        const QString &infoString)
{
    // End-to-end deterministic derivation:
    // mnemonic/passphrase -> seed64 -> HKDF(infoString) -> seed32 -> Ed25519 keypair

    DerivedEd25519 d;

    // 1) Seed from mnemonic/passphrase
    const QByteArray root = bip39Seed64(mnemonicWords, passphrase);
    if (root.size() != 64) return d;

    // 2) Domain-separated seed for this particular key purpose/version
    d.seed32 = hkdfSha512_32(root, infoString.toUtf8());
    if (d.seed32.size() != 32) return d;

    // 3) Ed25519 keypair from raw private key seed
    //
    // OpenSSL expects 32 bytes for raw Ed25519 private key.
    // We never expose EVP_PKEY outside this function (keeps OpenSSL ownership local).
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(d.seed32.constData()),
        d.seed32.size());

    // NOTE: Returning {} here yields a default-constructed DerivedEd25519, consistent with failures.
    if (!pkey) return {};

    // 4) Extract raw public key (32 bytes)
    unsigned char pub[32];
    size_t pubLen = sizeof(pub);

    if (EVP_PKEY_get_raw_public_key(pkey, pub, &pubLen) != 1 || pubLen != 32) {
        EVP_PKEY_free(pkey);
        return {};
    }

    d.pub32 = QByteArray(reinterpret_cast<const char*>(pub), 32);

    // 5) Compose a 64-byte private key representation used by many Ed25519 toolchains:
    //    priv64 = seed32 || pub32
    //
    // This is NOT the OpenSSH file format; it’s the raw key material that the
    // serialization layer (OpenSshEd25519Key) can embed into OpenSSH’s container.
    d.priv64 = d.seed32 + d.pub32;

    EVP_PKEY_free(pkey);
    return d;
}