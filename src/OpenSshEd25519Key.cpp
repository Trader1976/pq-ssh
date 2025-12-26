#include "OpenSshEd25519Key.h"
#include <QRandomGenerator>

//
// ARCHITECTURE NOTES (OpenSshEd25519Key.cpp)
//
// This module is a tiny, self-contained "format adapter":
// it takes raw Ed25519 key material (pub32, priv64) and emits OpenSSH-compatible
// representations:
//
//   1) Public key line:  "ssh-ed25519 <base64-blob> <comment>"
//   2) Private key file: OpenSSH "openssh-key-v1" PEM-like block
//
// Important boundaries:
// - NO key generation happens here.
// - NO passphrase/encryption happens here (ciphername/kdfname = "none").
// - NO network operations happen here.
// - The caller (IdentityDerivation / IdentityManagerDialog / future IdentityManager)
//   owns how pub32/priv64 are created and how the private key is protected at rest.
//
// Security intent:
// - We keep this code small and auditable because file formats are easy to get wrong.
// - The produced private key is intentionally unencrypted (OpenSSH "none/none") because
//   encryption at rest is handled by OpenSSH key format when generated with a passphrase (ssh-keygen -N).
//

// =====================================================
// File-local helpers: OpenSSH wire primitives
// =====================================================
//
// OpenSSH encodes many internal structures using SSH "string" format:
//   uint32 length (big-endian) || payload bytes
//
// Public key "blob" for ssh-ed25519 is:
//   string  "ssh-ed25519"
//   string  pubkey(32 bytes)
//
// Private key file ("openssh-key-v1") contains similar string fields and a key record.
//

// Append a big-endian uint32.
static void u32(QByteArray &b, quint32 v) {
    b.append(char((v >> 24) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

// Append an SSH "string": uint32 length + raw bytes.
static void sshStr(QByteArray &b, const QByteArray &s) {
    u32(b, s.size());
    b.append(s);
}

// Build the OpenSSH public key blob for an Ed25519 key.
// This blob is what gets base64-encoded in the authorized_keys line.
static QByteArray pubBlob(const QByteArray &pub32) {
    QByteArray b;
    sshStr(b, "ssh-ed25519"); // key type name
    sshStr(b, pub32);         // 32-byte raw public key
    return b;
}

// =====================================================
// Public key line (authorized_keys format)
// =====================================================
//
// Output format:
//   ssh-ed25519 <base64(pubBlob)> <comment>
//
// The comment is a user-facing label; it does not affect cryptographic validity.
//
QString OpenSshEd25519Key::publicKeyLine(const QByteArray &pub32,
                                        const QString &comment)
{
    // OpenSSH expects the base64-encoded "public key blob" as the second field.
    const QByteArray blob = pubBlob(pub32).toBase64();

    // Use a stable default comment for traceability.
    return QString("ssh-ed25519 %1 %2")
            .arg(QString::fromLatin1(blob),
                 comment.isEmpty() ? "pq-ssh" : comment);
}

// =====================================================
// Private key file (OpenSSH "openssh-key-v1" format)
// =====================================================
//
// The output is a PEM-like envelope:
//   -----BEGIN OPENSSH PRIVATE KEY-----
//   base64(openssh-key-v1 binary)
//   -----END OPENSSH PRIVATE KEY-----
//
// In this v1 implementation we deliberately emit an UNENCRYPTED OpenSSH private key:
//   ciphername = "none"
//   kdfname    = "none"
//   kdfoptions = empty
//
// The caller is responsible for:
// - restricting filesystem permissions (0600)
// - optionally encrypting at rest using pq-ssh's own key vault mechanism
//
// NOTE on inputs:
// - pub32  must be 32 bytes
// - priv64 should be "seed32 || pub32" (64 bytes) for OpenSSH Ed25519 keys.
//
QByteArray OpenSshEd25519Key::privateKeyFile(const QByteArray &pub32,
                                            const QByteArray &priv64,
                                            const QString &comment)
{
    QByteArray key;

    // ---- Header: "openssh-key-v1\0" ----
    // OpenSSH uses a C-string marker including a NUL terminator.
    // We append 15 bytes: 14 chars + '\0'
    key.append("openssh-key-v1\0", 15);

    // ---- Encryption/KDF fields ----
    // ciphername: "none" -> plaintext key
    // kdfname   : "none"
    // kdfoptions: empty string
    sshStr(key, "none");
    sshStr(key, "none");
    sshStr(key, {});

    // number of keys (we store exactly one)
    u32(key, 1);

    // ---- Public key(s) section ----
    // Each key is stored as an SSH string containing the public blob.
    QByteArray pb = pubBlob(pub32);
    sshStr(key, pb);

    // ---- Private key record ----
    // OpenSSH key record begins with two identical checkints.
    // They are used to detect incorrect passphrases when encrypted.
    // Even for "none", OpenSSH still expects them.
    QByteArray rec;
    quint32 chk = QRandomGenerator::global()->generate();
    u32(rec, chk);
    u32(rec, chk);

    // Then the actual key data (type + pub + priv + comment)
    sshStr(rec, "ssh-ed25519");
    sshStr(rec, pub32);
    sshStr(rec, priv64);

    // Comment is stored as bytes; default to "pq-ssh" for traceability.
    const QByteArray cmt = comment.isEmpty() ? QByteArray("pq-ssh") : comment.toUtf8();
    sshStr(rec, cmt);

    // OpenSSH pads the private key record to a multiple of 8 bytes using 1,2,3,...
    // This is an OpenSSH convention and is required for compatibility.
    int pad = 8 - (rec.size() % 8);
    if (pad == 8) pad = 0;
    for (int i = 1; i <= pad; ++i)
        rec.append(char(i));

    // The record is stored as an SSH string inside the top-level key structure.
    sshStr(key, rec);

    // ---- ASCII armor ----
    // OpenSSH private key blocks use base64 with lines typically wrapped (70 chars here).
    QByteArray b64 = key.toBase64();
    QByteArray out("-----BEGIN OPENSSH PRIVATE KEY-----\n");
    for (int i = 0; i < b64.size(); i += 70)
        out.append(b64.mid(i, 70)).append('\n');
    out.append("-----END OPENSSH PRIVATE KEY-----\n");
    return out;
}