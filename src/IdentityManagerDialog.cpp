// IdentityManagerDialog.cpp
//
// PURPOSE
// -------
// UI for deriving a deterministic “global” SSH keypair from a human-memorable
// recovery phrase (24 words) + optional passphrase.
//
// This dialog is intentionally small and self-contained:
//  - It collects the inputs (words, passphrase, comment)
//  - Calls IdentityDerivation to derive an Ed25519 keypair deterministically
//  - Renders an OpenSSH-compatible public key line and private key file
//  - Provides copy/save utilities
//
// ARCHITECTURAL NOTES
// -------------------
// - IdentityDerivation encapsulates the key derivation algorithm (KDF, domain separation, etc.).
// - OpenSshEd25519Key encapsulates OpenSSH serialization details.
// - This dialog should avoid implementing crypto primitives directly; it only orchestrates.
//
// SECURITY MODEL (HIGH LEVEL)
// ---------------------------
// - The 24 words + passphrase are *secrets*. Treat them as highly sensitive.
// - The derived private key (m_priv64 / m_privFile) is also a secret.
// - We avoid writing anything to disk unless the user explicitly clicks “Save private…”.
// - We avoid logging secrets (no qDebug on inputs/outputs).
//
// IMPORTANT SECURITY FOOTGUNS (TO WATCH)
// -------------------------------------
// - Secrets live in Qt QString/QByteArray, which are not “secure zeroed” containers.
//   This is acceptable for now, but for hardened builds consider:
//   - Minimizing lifetime of secret buffers
//   - Explicitly clearing buffers after use
// - Clipboard operations leak data to the OS clipboard history; user intent is explicit here.
// - Deterministic key derivation means phrase compromise => key compromise.
//   Domain separation string ("info") is critical to prevent cross-protocol key reuse.
//
// NOTE ON “FINGERPRINT”
// ---------------------
// OpenSSH typically uses SHA256(base64) style fingerprints for keys.
// Here we compute SHA3-512 hex over the raw 32-byte public key.
// That’s fine as an internal fingerprint, but be explicit in the UI/docs that this
// is a “PQ-SSH fingerprint” (not the OpenSSH default) to reduce user confusion.

#include "IdentityManagerDialog.h"
#include "IdentityDerivation.h"
#include "OpenSshEd25519Key.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QClipboard>
#include <QApplication>
#include <QFile>
#include <QMessageBox>

#include <openssl/evp.h>

IdentityManagerDialog::IdentityManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    // UI shell
    setWindowTitle("Identity Manager");
    resize(700, 520);

    // Layout: top form -> derive button -> fingerprint label -> public output -> action row
    auto *v = new QVBoxLayout(this);
    auto *f = new QFormLayout();

    // Recovery words (expected: 24 tokens). We accept free-form text and let
    // IdentityDerivation decide how to normalize/parse it.
    m_words = new QPlainTextEdit;
    m_words->setPlaceholderText("word1 word2 ... word24");

    // Optional passphrase (BIP39-like “25th word” concept). This is a secret.
    m_pass = new QLineEdit;
    m_pass->setEchoMode(QLineEdit::Password);

    // Comment is appended to authorized_keys line. Not security relevant, but helps UX.
    m_comment = new QLineEdit("pq-ssh");

    f->addRow("24 words:", m_words);
    f->addRow("Passphrase:", m_pass);
    f->addRow("Comment:", m_comment);
    v->addLayout(f);

    // Derivation trigger.
    // Note: Derivation should be fast; if it becomes slow, move to a worker thread
    // and disable UI while running to avoid re-entrancy.
    auto *btn = new QPushButton("Derive global SSH key");
    connect(btn, &QPushButton::clicked, this, &IdentityManagerDialog::onDerive);
    v->addWidget(btn);

    // Fingerprint display. Starts blank-ish; updated after successful derive.
    m_fp = new QLabel("Fingerprint:");
    v->addWidget(m_fp);

    // Public key output (authorized_keys line). Read-only: user copies/saves from here.
    m_pubOut = new QPlainTextEdit;
    m_pubOut->setReadOnly(true);
    v->addWidget(m_pubOut, 1);

    // Actions row: clipboard + save to disk
    auto *row = new QHBoxLayout();
    auto *cp = new QPushButton("Copy public");
    auto *cf = new QPushButton("Copy fingerprint");
    auto *sp = new QPushButton("Save private…");
    auto *su = new QPushButton("Save public…");

    connect(cp, &QPushButton::clicked, this, &IdentityManagerDialog::onCopyPublic);
    connect(cf, &QPushButton::clicked, this, &IdentityManagerDialog::onCopyFingerprint);
    connect(sp, &QPushButton::clicked, this, &IdentityManagerDialog::onSavePrivate);
    connect(su, &QPushButton::clicked, this, &IdentityManagerDialog::onSavePublic);

    row->addWidget(cp);
    row->addWidget(cf);
    row->addWidget(sp);
    row->addWidget(su);
    v->addLayout(row);
}

QString IdentityManagerDialog::sha3Fingerprint(const QByteArray &pub32)
{
    // Computes a deterministic “PQ-SSH fingerprint” as:
    //   SHA3-512(pubkey_raw_32_bytes) -> hex string (128 hex chars)
    //
    // Why SHA3-512?
    // - Strong hash and aligns with “PQ-ish” branding.
    // - Note: This is NOT the same as OpenSSH’s default fingerprint display.
    //
    // Input assumptions:
    // - pub32 should be exactly 32 bytes for Ed25519 public key.

    unsigned char out[64];
    unsigned int len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    // NOTE: In hardened code, always check ctx != nullptr and DigestInit return values.
    EVP_DigestInit_ex(ctx, EVP_sha3_512(), nullptr);
    EVP_DigestUpdate(ctx, pub32.constData(), pub32.size());
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);

    // Render lowercase hex, 2 chars per byte.
    QString h;
    for (int i = 0; i < 64; i++)
        h += QString("%1").arg(out[i], 2, 16, QLatin1Char('0'));
    return h;
}

void IdentityManagerDialog::onDerive()
{
    // Domain separation / context binding.
    // This string ensures the same words+passphrase won’t accidentally be reused across:
    //  - different apps
    //  - different key purposes
    //  - future format changes (versioned suffix)
    //
    // If you ever introduce per-profile keys (host/user scoped), extend the info string
    // with normalized identifiers (e.g., host:port:user) to derive *distinct* keys.
    const QString info = "CPUNK/PQSSH/ssh-ed25519/global/v1";

    // Derive deterministic Ed25519 keypair from inputs.
    // IdentityDerivation is expected to handle:
    // - normalization (trim, multiple spaces, newlines)
    // - word count validation (if desired)
    // - KDF and key generation
    //
    // d.pub32: 32-byte Ed25519 public key
    // d.priv64: 64-byte Ed25519 private key material (implementation-specific format)
    auto d = IdentityDerivation::deriveEd25519FromWords(
        m_words->toPlainText(),
        m_pass->text(),
        info);

    // Basic sanity guard. If derivation fails, we do not update UI.
    // (Consider showing a QMessageBox with a helpful error in the future.)
    if (d.pub32.size() != 32) return;

    // Cache raw key material in memory for later operations (save/private file creation).
    // NOTE: These buffers are secrets. Consider minimizing lifetime or clearing on close.
    m_pub32 = d.pub32;
    m_priv64 = d.priv64;

    // Display fingerprint (internal/PQ-SSH style).
    m_fp->setText("Fingerprint: " + sha3Fingerprint(m_pub32));

    // Build the OpenSSH public key line:
    //   "ssh-ed25519 AAAAC3... comment"
    const QString pubLine =
        OpenSshEd25519Key::publicKeyLine(m_pub32, m_comment->text());

    m_pubOut->setPlainText(pubLine);

    // Build the OpenSSH private key file blob (PEM-like OpenSSH format).
    // This is kept in memory until the user saves it.
    m_privFile =
        OpenSshEd25519Key::privateKeyFile(m_pub32, m_priv64, m_comment->text());
}

void IdentityManagerDialog::onCopyPublic()
{
    // Copies the full authorized_keys line to clipboard.
    // WARNING: Clipboard is global OS state. This is intentional by user action.
    QApplication::clipboard()->setText(m_pubOut->toPlainText());
}

void IdentityManagerDialog::onCopyFingerprint()
{
    // UI label format is "Fingerprint: <hex>".
    // We remove the prefix so user gets only the hex value.
    // NOTE: This assumes prefix length is stable; if UI text changes, update this.
    QApplication::clipboard()->setText(m_fp->text().mid(12));
}

void IdentityManagerDialog::onSavePrivate()
{
    // Private key is only available after derive().
    if (m_privFile.isEmpty()) {
        QMessageBox::warning(this, "Save private key", "No private key derived yet.");
        return;
    }

    // Default filename follows OpenSSH convention.
    const QString p = QFileDialog::getSaveFileName(this, "Save private key", "id_ed25519");
    if (p.isEmpty()) return;

    // Write file atomically-ish:
    // - This writes directly to the target file. For stronger safety, consider writing to
    //   a temp file then renaming (QSaveFile).
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Save private key", "Cannot write file:\n" + f.errorString());
        return;
    }

    const qint64 written = f.write(m_privFile);
    f.close();

    // Ensure OpenSSH accepts it: private key must not be accessible by others.
    //
    // IMPORTANT BUG NOTE:
    // The current code calls setPermissions twice; the second call overwrites the first.
    // If you wanted 0600 (read+write owner), do a SINGLE call with both flags.
    //
    // Also: QSaveFile would preserve permissions more predictably in some cases.
    QFile::setPermissions(p, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::setPermissions(p, QFileDevice::ReadOwner);

    if (written != m_privFile.size()) {
        QMessageBox::critical(this, "Save private key", "Write failed (short write).");
        return;
    }
}

void IdentityManagerDialog::onSavePublic()
{
    // Public key line must exist after derive().
    const QString pubLine = m_pubOut ? m_pubOut->toPlainText().trimmed() : QString();
    if (pubLine.isEmpty()) {
        QMessageBox::warning(this, "Save public key", "No public key derived yet.");
        return;
    }

    // Default filename follows OpenSSH convention.
    const QString p = QFileDialog::getSaveFileName(this, "Save public key", "id_ed25519.pub");
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Save public key", "Cannot write file:\n" + f.errorString());
        return;
    }

    // Ensure newline at end so it can be appended safely to authorized_keys.
    const QByteArray bytes = (pubLine + "\n").toUtf8();
    const qint64 written = f.write(bytes);
    f.close();

    if (written != bytes.size()) {
        QMessageBox::critical(this, "Save public key", "Write failed (short write).");
        return;
    }
}