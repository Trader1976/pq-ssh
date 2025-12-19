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
    setWindowTitle("Identity Manager");
    resize(700, 520);

    auto *v = new QVBoxLayout(this);
    auto *f = new QFormLayout();

    m_words = new QPlainTextEdit;
    m_words->setPlaceholderText("word1 word2 ... word24");

    m_pass = new QLineEdit;
    m_pass->setEchoMode(QLineEdit::Password);

    m_comment = new QLineEdit("pq-ssh");

    f->addRow("24 words:", m_words);
    f->addRow("Passphrase:", m_pass);
    f->addRow("Comment:", m_comment);
    v->addLayout(f);

    auto *btn = new QPushButton("Derive global SSH key");
    connect(btn, &QPushButton::clicked, this, &IdentityManagerDialog::onDerive);
    v->addWidget(btn);

    m_fp = new QLabel("Fingerprint:");
    v->addWidget(m_fp);

    m_pubOut = new QPlainTextEdit;
    m_pubOut->setReadOnly(true);
    v->addWidget(m_pubOut, 1);

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
    unsigned char out[64];
    unsigned int len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha3_512(), nullptr);
    EVP_DigestUpdate(ctx, pub32.constData(), pub32.size());
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);

    QString h;
    for (int i = 0; i < 64; i++)
        h += QString("%1").arg(out[i], 2, 16, QLatin1Char('0'));
    return h;
}

void IdentityManagerDialog::onDerive()
{
    const QString info = "CPUNK/PQSSH/ssh-ed25519/global/v1";

    auto d = IdentityDerivation::deriveEd25519FromWords(
        m_words->toPlainText(),
        m_pass->text(),
        info);

    if (d.pub32.size() != 32) return;

    m_pub32 = d.pub32;
    m_priv64 = d.priv64;

    m_fp->setText("Fingerprint: " + sha3Fingerprint(m_pub32));

    const QString pubLine =
        OpenSshEd25519Key::publicKeyLine(m_pub32, m_comment->text());

    m_pubOut->setPlainText(pubLine);
    m_privFile =
        OpenSshEd25519Key::privateKeyFile(m_pub32, m_priv64, m_comment->text());
}

void IdentityManagerDialog::onCopyPublic()
{
    QApplication::clipboard()->setText(m_pubOut->toPlainText());
}

void IdentityManagerDialog::onCopyFingerprint()
{
    QApplication::clipboard()->setText(m_fp->text().mid(12));
}

void IdentityManagerDialog::onSavePrivate()
{
    if (m_privFile.isEmpty()) {
        QMessageBox::warning(this, "Save private key", "No private key derived yet.");
        return;
    }

    const QString p = QFileDialog::getSaveFileName(this, "Save private key", "id_ed25519");
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Save private key", "Cannot write file:\n" + f.errorString());
        return;
    }

    const qint64 written = f.write(m_privFile);
    f.close();

    // Ensure OpenSSH accepts it: private key must not be accessible by others
    QFile::setPermissions(p, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::setPermissions(p, QFileDevice::ReadOwner);

    if (written != m_privFile.size()) {
        QMessageBox::critical(this, "Save private key", "Write failed (short write).");
        return;
    }
}
void IdentityManagerDialog::onSavePublic()
{
    const QString pubLine = m_pubOut ? m_pubOut->toPlainText().trimmed() : QString();
    if (pubLine.isEmpty()) {
        QMessageBox::warning(this, "Save public key", "No public key derived yet.");
        return;
    }

    const QString p = QFileDialog::getSaveFileName(this, "Save public key", "id_ed25519.pub");
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Save public key", "Cannot write file:\n" + f.errorString());
        return;
    }

    const QByteArray bytes = (pubLine + "\n").toUtf8();
    const qint64 written = f.write(bytes);
    f.close();

    if (written != bytes.size()) {
        QMessageBox::critical(this, "Save public key", "Write failed (short write).");
        return;
    }
}