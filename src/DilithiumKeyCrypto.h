#pragma once
#include <QByteArray>
#include <QString>

// =====================================================
// DilithiumKeyCrypto
// =====================================================
//
// Purpose
// -------
// Small, dependency-light crypto helper for PQ-SSH’s *encrypted-at-rest*
// Dilithium private key blobs.
//
// What it DOES
// ------------
// - Encrypts a raw private-key byte blob using a user passphrase.
// - Decrypts an encrypted blob back into plaintext bytes.
//
// What it does NOT do
// -------------------
// - It does not generate Dilithium keys.
// - It does not perform SSH authentication or network operations.
// - It does not read/write files; callers own I/O.
//
// Security notes
// --------------
// - Caller MUST treat `plain` / `outPlain` as highly sensitive.
// - This module never logs secrets; do not add logging of key material.
// - The returned plaintext is NOT auto-wiped; callers must wipe it ASAP.
//   (Use sodium_memzero() or similar in the caller once done.)
// - On failure, `outEncrypted` / `outPlain` should be considered invalid.
//
// Format notes
// ------------
// - The encrypted blob uses an internal header + ciphertext layout.
// - The header begins with a fixed MAGIC/version string (e.g. "PQSSH1").
// - If the on-disk format changes, bump MAGIC and keep backward support
//   at a higher layer (migration strategy).
//

// Encrypts `plain` using `passphrase` and writes the encrypted blob to `outEncrypted`.
// Returns true on success, false on failure (details optionally in `err`).
bool encryptDilithiumKey(
    const QByteArray& plain,
    const QString& passphrase,
    QByteArray* outEncrypted,
    QString* err
);

// Decrypts `encrypted` using `passphrase` and writes plaintext bytes to `outPlain`.
// Returns true on success, false on failure (details optionally in `err`).
//
// IMPORTANT: Caller must wipe `outPlain` as soon as possible after use.
bool decryptDilithiumKey(
    const QByteArray& encrypted,
    const QString& passphrase,
    QByteArray* outPlain,
    QString* err
);
