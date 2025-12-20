#pragma once
//
// IdentityManagerDialog.h
//
// PURPOSE
// -------
// IdentityManagerDialog is a small, self-contained UI component that lets the user
// derive a deterministic SSH keypair from a recovery phrase (24 words) plus an
// optional passphrase.
//
// It is designed as an "identity on-ramp":
// - User enters words + passphrase
// - pq-ssh derives a stable Ed25519 keypair (see IdentityDerivation)
// - Dialog displays a public key line suitable for ~/.ssh/authorized_keys
// - Dialog can copy/save public + private material on explicit user actions
//
// ARCHITECTURAL BOUNDARIES
// ------------------------
// This dialog is an orchestrator:
// - It does NOT implement key derivation; that belongs to IdentityDerivation.
// - It does NOT implement OpenSSH key formatting; that belongs to OpenSshEd25519Key.
// - It does NOT manage app-wide identity storage; it only produces/copies/saves.
//
// SECURITY NOTES (IMPORTANT)
// --------------------------
// Inputs and outputs handled here are highly sensitive:
// - m_words (recovery phrase) and m_pass (passphrase) are secrets.
// - m_priv64 and m_privFile contain private key material.
// This class intentionally avoids logging secrets.
//
// Clipboard operations are user-triggered but inherently leak data to the OS clipboard.
// Saving the private key writes to disk; permissions must be restricted so OpenSSH accepts it.
//
// FUTURE EXTENSIONS
// -----------------
// - Add validation (word count, normalization feedback, error messages).
// - Add "scoped" identities (per-host / per-profile derivation) using domain separation.
// - Consider secure memory handling / explicit clearing of secret buffers on close.

#include <QDialog>

class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;

class IdentityManagerDialog : public QDialog
{
    Q_OBJECT
public:
    // Constructs the dialog and builds its UI.
    // Derivation is performed only when the user clicks “Derive …”.
    explicit IdentityManagerDialog(QWidget *parent = nullptr);

private slots:
    // Derives the keypair from (words + passphrase + context string),
    // updates fingerprint, public key output, and in-memory private key blob.
    void onDerive();

    // Copies the public authorized_keys line to the OS clipboard.
    void onCopyPublic();

    // Copies the displayed fingerprint (without the “Fingerprint:” prefix) to clipboard.
    void onCopyFingerprint();

    // Saves the derived private key file blob to disk with restrictive permissions.
    // (Only available after successful derivation.)
    void onSavePrivate();

    // Saves the derived public key line to disk (adds trailing newline).
    void onSavePublic();

private:
    // Computes a deterministic “PQ-SSH fingerprint” as SHA3-512(pubkey32) in hex.
    // Note: This is not OpenSSH’s default SHA256/base64 fingerprint display.
    QString sha3Fingerprint(const QByteArray &pub32);

    // === UI widgets (owned by Qt parent hierarchy) ===
    QPlainTextEdit *m_words   = nullptr;  // recovery phrase input (secret)
    QLineEdit      *m_pass    = nullptr;  // passphrase input (secret)
    QLineEdit      *m_comment = nullptr;  // public key comment (non-secret)
    QLabel         *m_fp      = nullptr;  // fingerprint display
    QPlainTextEdit *m_pubOut  = nullptr;  // public key output (authorized_keys line)

    // === Derived materials (in-memory) ===
    QByteArray m_pub32;    // 32-byte Ed25519 public key (raw)
    QByteArray m_priv64;   // 64-byte Ed25519 private key material (format defined by derivation/key module)
    QByteArray m_privFile; // OpenSSH private key file content (ready to write)
};