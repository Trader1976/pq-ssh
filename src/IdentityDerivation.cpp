#include "IdentityDerivation.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

static QByteArray norm(const QString &s)
{
    return s.simplified().toUtf8();
}

QByteArray IdentityDerivation::bip39Seed64(const QString &mnemonicWords,
                                          const QString &passphrase)
{
    QByteArray out(64, 0);
    const QByteArray m = norm(mnemonicWords);
    const QByteArray salt = QByteArray("mnemonic") + norm(passphrase);

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
    unsigned char prk[64];
    unsigned int prkLen = 0;
    unsigned char zeroSalt[64] = {0};

    HMAC(EVP_sha512(),
         zeroSalt, sizeof(zeroSalt),
         reinterpret_cast<const unsigned char*>(ikm.constData()), ikm.size(),
         prk, &prkLen);

    if (prkLen != 64) return {};

    QByteArray msg = info;
    msg.append(char(0x01));

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
    DerivedEd25519 d;

    const QByteArray root = bip39Seed64(mnemonicWords, passphrase);
    if (root.size() != 64) return d;

    d.seed32 = hkdfSha512_32(root, infoString.toUtf8());
    if (d.seed32.size() != 32) return d;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(d.seed32.constData()),
        d.seed32.size());

    if (!pkey) return {};

    unsigned char pub[32];
    size_t pubLen = sizeof(pub);

    if (EVP_PKEY_get_raw_public_key(pkey, pub, &pubLen) != 1 || pubLen != 32) {
        EVP_PKEY_free(pkey);
        return {};
    }

    d.pub32 = QByteArray(reinterpret_cast<const char*>(pub), 32);
    d.priv64 = d.seed32 + d.pub32;

    EVP_PKEY_free(pkey);
    return d;
}
