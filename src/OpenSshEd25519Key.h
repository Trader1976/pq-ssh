#pragma once
#include <QByteArray>
#include <QString>

//
// ARCHITECTURE NOTES (OpenSshEd25519Key.h)
//
// This header defines a very small, format-focused API for emitting
// OpenSSH-compatible Ed25519 keys from *already-derived* key material.
//
// Design goals:
// - Keep the surface area minimal and explicit.
// - Avoid any hidden crypto, randomness (except OpenSSH-required checkints),
//   or filesystem behavior.
// - Make it obvious that this is a *formatter*, not a key generator.
//
// Responsibility boundaries:
// - Callers provide raw key material:
//     * pub32  = 32-byte Ed25519 public key
//     * priv64 = 64-byte private key material (seed || pub, per OpenSSH)
// - Callers decide:
//     * how keys are derived (mnemonics, identity manager, hardware, etc.)
//     * how private keys are encrypted at rest (if at all)
//     * where and how files are written (permissions, storage location)
//
// This module:
// - Produces OpenSSH wire formats only.
// - Does not validate cryptographic correctness beyond structure.
// - Does not enforce security policy (that lives at higher layers).
//

namespace OpenSshEd25519Key {

    // -------------------------------------------------
    // Public key (authorized_keys format)
    // -------------------------------------------------
    //
    // Returns a single-line OpenSSH public key:
    //
    //   ssh-ed25519 <base64(public-blob)> <comment>
    //
    // The comment is optional and has no cryptographic meaning;
    // it is included for user identification and tooling clarity.
    //
    // Expected inputs:
    //   pub32   - exactly 32 bytes (Ed25519 public key)
    //   comment - optional user-visible label
    //
    // The returned string is suitable for:
    //   - ~/.ssh/authorized_keys
    //   - ssh-add
    //   - server-side key installation
    //
    QString publicKeyLine(const QByteArray &pub32,
                          const QString &comment);

    // -------------------------------------------------
    // Private key file (OpenSSH "openssh-key-v1" format)
    // -------------------------------------------------
    //
    // Returns a complete OpenSSH private key file, including:
    //
    //   -----BEGIN OPENSSH PRIVATE KEY-----
    //   <base64 data>
    //   -----END OPENSSH PRIVATE KEY-----
    //
    // Important security notes:
    // - The key is emitted UNENCRYPTED (ciphername="none").
    // - Callers MUST ensure:
    //     * filesystem permissions (0600)
    //     * optional encryption at rest (handled elsewhere in pq-ssh)
    //
    // Expected inputs:
    //   pub32   - 32-byte Ed25519 public key
    //   priv64  - 64-byte private key material (seed32 || pub32)
    //   comment - stored inside the key for identification
    //
    // This function performs no I/O and no persistent storage.
    //
    QByteArray privateKeyFile(const QByteArray &pub32,
                              const QByteArray &priv64,
                              const QString &comment);

} // namespace OpenSshEd25519Key