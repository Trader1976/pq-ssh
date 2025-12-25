// IdentityManagerDialog.cpp
//
// NOTE: This version removes the broken dependency on bip39MasterSeed64()
// and uses DnaIdentityDerivation::bip39Seed64() (which you *do* have in the namespace).
// It also removes the unused extern shake256 symbol (you already use OpenSSL for SHAKE in the derivation module).
//
// If you *really* want to keep using the Dilithium vendor shake symbol, keep the extern and call,
// but then make sure it exists exactly once. Right now you don’t need it here.

#include "IdentityManagerDialog.h"
#include "OpenSshEd25519Key.h"
#include "DnaIdentityDerivation.h"

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

static void bestEffortZero(QByteArray &b)
{
    if (b.isEmpty()) return;
    // Best-effort wipe (Qt containers aren’t guaranteed secure)
    memset(b.data(), 0, static_cast<size_t>(b.size()));
    b.clear();
}

IdentityManagerDialog::IdentityManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Identity Manager"));
    resize(700, 520);

    auto *v = new QVBoxLayout(this);
    auto *f = new QFormLayout();

    m_words = new QPlainTextEdit;
    m_words->setPlaceholderText(tr("word1 word2 ... word24"));

    m_pass = new QLineEdit;
    m_pass->setEchoMode(QLineEdit::Password);

    m_comment = new QLineEdit(QStringLiteral("pq-ssh"));

    f->addRow(tr("24 words:"), m_words);
    f->addRow(tr("Passphrase:"), m_pass);
    f->addRow(tr("Comment:"), m_comment);
    v->addLayout(f);

    auto *btn = new QPushButton(tr("Derive identity (DNA → SSH key)"));
    connect(btn, &QPushButton::clicked, this, [this]{
        const QString words = m_words ? m_words->toPlainText() : QString();
        const QString pass  = m_pass  ? m_pass->text()         : QString();

        // 1) DNA identity (fingerprint)
        //    This MUST exist and link: DnaIdentityDerivation::deriveFromWords(words, pass)
        const auto r = DnaIdentityDerivation::deriveFromWords(words, pass);
        if (!r.ok) {
            QMessageBox::warning(this, tr("Identity"), tr("Failed: %1").arg(r.error));
            return;
        }
        m_fp->setText(tr("Fingerprint: %1").arg(r.fingerprintHex128));

        // 2) Derive Ed25519 keypair from SAME BIP39 master seed, but with domain separation.
        //
        // master64 = PBKDF2-HMAC-SHA512(mnemonic, salt="mnemonic"+pass, 2048, 64)
        // edSeed32 = SHAKE256(master64 || "cpunk-pqssh-ed25519-v1", 32)
        //
        // Use your existing DnaIdentityDerivation::bip39Seed64 (no new API).
        QByteArray master64 = DnaIdentityDerivation::bip39Seed64(words, pass);
        if (master64.size() != 64) {
            QMessageBox::critical(this, tr("Identity"), tr("Failed to derive BIP39 master seed."));
            return;
        }

        const QByteArray ctx = QByteArrayLiteral("cpunk-pqssh-ed25519-v1");
        QByteArray in = master64 + ctx;

        // Use OpenSSL SHAKE256 (no dependency on Dilithium vendor shake symbol)
        QByteArray edSeed32 = DnaIdentityDerivation::shake256_32(in);
        if (edSeed32.size() != 32) {
            bestEffortZero(master64);
            bestEffortZero(in);
            QMessageBox::critical(this, tr("Identity"), tr("Failed to derive Ed25519 seed (SHAKE256)."));
            return;
        }

        // Generate Ed25519 pub32 from seed32 using OpenSSL raw API
        EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(edSeed32.constData()),
            static_cast<size_t>(edSeed32.size())
        );
        if (!pkey) {
            bestEffortZero(master64);
            bestEffortZero(in);
            bestEffortZero(edSeed32);
            QMessageBox::critical(this, tr("Identity"), tr("Failed to create Ed25519 key."));
            return;
        }

        unsigned char pub[32];
        size_t pubLen = sizeof(pub);
        if (EVP_PKEY_get_raw_public_key(pkey, pub, &pubLen) != 1 || pubLen != 32) {
            EVP_PKEY_free(pkey);
            bestEffortZero(master64);
            bestEffortZero(in);
            bestEffortZero(edSeed32);
            QMessageBox::critical(this, tr("Identity"), tr("Failed to extract Ed25519 public key."));
            return;
        }
        EVP_PKEY_free(pkey);

        // Cache key material for Save/Copy actions
        m_pub32  = QByteArray(reinterpret_cast<const char*>(pub), 32);
        m_priv64 = edSeed32 + m_pub32;

        const QString comment = m_comment ? m_comment->text() : QStringLiteral("pq-ssh");
        const QString pubLine = OpenSshEd25519Key::publicKeyLine(m_pub32, comment);
        m_pubOut->setPlainText(pubLine);
        m_privFile = OpenSshEd25519Key::privateKeyFile(m_pub32, m_priv64, comment);

        // Best-effort wipe temporary secrets
        bestEffortZero(master64);
        bestEffortZero(in);
        bestEffortZero(edSeed32);
    });
    v->addWidget(btn);

    m_fp = new QLabel(tr("Fingerprint:"));
    v->addWidget(m_fp);

    m_pubOut = new QPlainTextEdit;
    m_pubOut->setReadOnly(true);
    v->addWidget(m_pubOut, 1);

    auto *row = new QHBoxLayout();
    auto *cp = new QPushButton(tr("Copy public"));
    auto *cf = new QPushButton(tr("Copy fingerprint"));
    auto *sp = new QPushButton(tr("Save private…"));
    auto *su = new QPushButton(tr("Save public…"));

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

void IdentityManagerDialog::onCopyPublic()
{
    if (m_pubOut)
        QApplication::clipboard()->setText(m_pubOut->toPlainText());
}

void IdentityManagerDialog::onCopyFingerprint()
{
    if (m_fp)
        QApplication::clipboard()->setText(m_fp->text().mid(tr("Fingerprint: ").size()));
}

void IdentityManagerDialog::onSavePrivate()
{
    if (m_privFile.isEmpty()) {
        QMessageBox::warning(this, tr("Save private key"), tr("No private key derived yet."));
        return;
    }

    const QString p = QFileDialog::getSaveFileName(this, tr("Save private key"), tr("id_ed25519"));
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, tr("Save private key"), tr("Cannot write file:\n%1").arg(f.errorString()));
        return;
    }

    const qint64 written = f.write(m_privFile);
    f.close();

    QFile::setPermissions(p, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    if (written != m_privFile.size()) {
        QMessageBox::critical(this, tr("Save private key"), tr("Write failed (short write)."));
        return;
    }
}

void IdentityManagerDialog::onSavePublic()
{
    const QString pubLine = m_pubOut ? m_pubOut->toPlainText().trimmed() : QString();
    if (pubLine.isEmpty()) {
        QMessageBox::warning(this, tr("Save public key"), tr("No public key derived yet."));
        return;
    }

    const QString p = QFileDialog::getSaveFileName(this, tr("Save public key"), tr("id_ed25519.pub"));
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, tr("Save public key"), tr("Cannot write file:\n%1").arg(f.errorString()));
        return;
    }

    const QByteArray bytes = (pubLine + "\n").toUtf8();
    const qint64 written = f.write(bytes);
    f.close();

    if (written != bytes.size()) {
        QMessageBox::critical(this, tr("Save public key"), tr("Write failed (short write)."));
        return;
    }
}
void IdentityManagerDialog::onDerive()
{
    const QString words = m_words ? m_words->toPlainText() : QString();
    const QString pass  = m_pass  ? m_pass->text()         : QString();

    const auto r = DnaIdentityDerivation::deriveFromWords(words, pass);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Identity"), tr("Failed: %1").arg(r.error));
        return;
    }

    if (m_fp)
        m_fp->setText(tr("Fingerprint: %1").arg(r.fingerprintHex128));

    QByteArray master64 = DnaIdentityDerivation::bip39Seed64(words, pass);
    if (master64.size() != 64) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to derive BIP39 master seed."));
        return;
    }

    const QByteArray ctx = QByteArrayLiteral("cpunk-pqssh-ed25519-v1");
    QByteArray in = master64 + ctx;

    QByteArray edSeed32 = DnaIdentityDerivation::shake256_32(in);
    if (edSeed32.size() != 32) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to derive Ed25519 seed (SHAKE256)."));
        memset(master64.data(), 0, (size_t)master64.size());
        memset(in.data(), 0, (size_t)in.size());
        return;
    }

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(edSeed32.constData()),
        (size_t)edSeed32.size()
    );
    if (!pkey) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to create Ed25519 key."));
        memset(master64.data(), 0, (size_t)master64.size());
        memset(in.data(), 0, (size_t)in.size());
        memset(edSeed32.data(), 0, (size_t)edSeed32.size());
        return;
    }

    unsigned char pub[32];
    size_t pubLen = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &pubLen) != 1 || pubLen != 32) {
        EVP_PKEY_free(pkey);
        QMessageBox::critical(this, tr("Identity"), tr("Failed to extract Ed25519 public key."));
        memset(master64.data(), 0, (size_t)master64.size());
        memset(in.data(), 0, (size_t)in.size());
        memset(edSeed32.data(), 0, (size_t)edSeed32.size());
        return;
    }
    EVP_PKEY_free(pkey);

    m_pub32  = QByteArray(reinterpret_cast<const char*>(pub), 32);
    m_priv64 = edSeed32 + m_pub32;

    const QString comment = m_comment ? m_comment->text() : QStringLiteral("pq-ssh");
    if (m_pubOut)
        m_pubOut->setPlainText(OpenSshEd25519Key::publicKeyLine(m_pub32, comment));
    m_privFile = OpenSshEd25519Key::privateKeyFile(m_pub32, m_priv64, comment);

    // best-effort wipe
    memset(master64.data(), 0, (size_t)master64.size());
    memset(in.data(), 0, (size_t)in.size());
    memset(edSeed32.data(), 0, (size_t)edSeed32.size());
}
