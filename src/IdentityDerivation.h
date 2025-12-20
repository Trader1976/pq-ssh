#pragma once
//
// IdentityDerivation.h
//
// PURPOSE
// -------
// This header defines the deterministic identity/key derivation interface used by pq-ssh.
// It converts human-memorable input (mnemonic words + optional passphrase) into
// cryptographic key material in a reproducible, domain-separated way.
//
// The API is intentionally small and explicit:
// - No UI concepts
// - No file formats
// - No SSH/network logic
// Just pure derivation primitives and a clear data container.
//
// ARCHITECTURAL POSITION
// ----------------------
// IdentityDerivation sits at the “crypto derivation” layer:
//
//   [ UI / Dialogs ]
//           ↓
//   [ IdentityDerivation ]  <-- this header
//           ↓
//   [ Key serialization (OpenSshEd25519Key) ]
//           ↓
//   [ SSH / libssh / OpenSSH ]
//
// This separation ensures:
// - Crypto logic is auditable and testable
// - UI changes do not affect derivation correctness
// - Future PQ or alternative algorithms can be added cleanly
//
// SECURITY MODEL
// --------------
// All inputs and outputs here are sensitive:
// - mnemonicWords and passphrase are secrets
// - seed32, priv64 are private key material
//
// Callers MUST:
// - Avoid logging these values
// - Minimize lifetime in memory where possible
// - Treat deterministic output as long-lived identity material
//
// DOMAIN SEPARATION (CRITICAL)
// ----------------------------
// The infoString parameter is *mandatory* for safe use.
// It binds derived keys to a specific purpose, protocol, and version.
//
// Example:
//   "CPUNK/PQSSH/ssh-ed25519/global/v1"
//
// Changing infoString WILL change the derived key.
// Reusing infoString across different purposes risks cross-protocol key reuse.

#include <QByteArray>
#include <QString>

struct DerivedEd25519 {
    // 32-byte Ed25519 private seed.
    // This is the core secret from which the full keypair is deterministically derived.
    QByteArray seed32;

    // 32-byte Ed25519 public key corresponding to seed32.
    QByteArray pub32;

    // 64-byte Ed25519 private key representation:
    //   priv64 = seed32 || pub32
    //
    // This layout is commonly used by Ed25519 toolchains and is
    // consumed by the OpenSSH serialization layer in pq-ssh.
    //
    // NOTE:
    // This is *not* the OpenSSH private key file format itself.
    // It is raw key material that must be wrapped/encoded elsewhere.
    QByteArray priv64;
};

namespace IdentityDerivation {

    // Derives a 64-byte root seed from mnemonic words + passphrase
    // using a BIP39-compatible PBKDF2-HMAC-SHA512 construction.
    //
    // - mnemonicWords: user-provided recovery phrase (not validated here)
    // - passphrase: optional extra secret ("25th word")
    //
    // Returns:
    // - 64-byte seed on success
    // - empty QByteArray on failure
    QByteArray bip39Seed64(const QString &mnemonicWords,
                           const QString &passphrase);

    // Minimal HKDF-SHA512 helper that derives a 32-byte output key
    // from input keying material (IKM) and a context string.
    //
    // This is used for domain separation and purpose binding.
    //
    // - ikm: input keying material (typically 64-byte seed)
    // - info: context / domain separation string (UTF-8)
    //
    // Returns:
    // - 32-byte derived key on success
    // - empty QByteArray on failure
    QByteArray hkdfSha512_32(const QByteArray &ikm,
                             const QByteArray &info);

    // High-level convenience function that performs full deterministic derivation:
    //
    //   mnemonic + passphrase
    //        ↓
    //   bip39Seed64()
    //        ↓
    //   hkdfSha512_32(infoString)
    //        ↓
    //   Ed25519 keypair
    //
    // infoString MUST be stable and versioned to ensure:
    // - safe domain separation
    // - reproducible identity recovery
    //
    // Returns:
    // - DerivedEd25519 with all fields populated on success
    // - Default-constructed DerivedEd25519 on failure
    DerivedEd25519 deriveEd25519FromWords(const QString &mnemonicWords,
                                          const QString &passphrase,
                                          const QString &infoString);

} // namespace IdentityDerivation