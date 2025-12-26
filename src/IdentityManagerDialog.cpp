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
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QResizeEvent>
#include <QListWidgetItem>
#include <QFontMetrics>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QRandomGenerator>

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
    resize(1100, 650);
    setMinimumWidth(1000);

    buildUi();
    loadSaved();
    clearDerivedUi();
}


void IdentityManagerDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10,10,10,10);
    root->setSpacing(10);

    auto *split = new QSplitter(this);
    split->setOrientation(Qt::Horizontal);

    // ===== Left: saved identities =====
    auto *left = new QWidget(split);

    const int leftW = 320;                 // pick what you like
    left->setMinimumWidth(leftW);
    left->setMaximumWidth(leftW);
    left->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    // Keep left pane width stable (prevents it from changing when right side updates)
    //left->setMinimumWidth(280);
    //left->setMaximumWidth(340); // adjust to taste
    //left->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    split->setCollapsible(0, false);
    split->setSizes({320, 1200});

    split->setHandleWidth(1);              // optional: thinner divider
    split->setChildrenCollapsible(false);  // prevents auto-collapse behavior

    auto *leftL = new QVBoxLayout(left);
    leftL->setContentsMargins(0,0,0,0);
    leftL->setSpacing(6);

    auto *leftTitle = new QLabel(tr("Saved identities"), left);
    leftTitle->setStyleSheet("font-weight:600;");
    leftL->addWidget(leftTitle);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({leftW, 2000});        // right side gets the rest

    m_savedList = new QListWidget(left);
    m_savedList->setSelectionMode(QAbstractItemView::SingleSelection);
    leftL->addWidget(m_savedList, 1);

    m_savedList->setToolTip(
    tr("Saved identities.\n"
       "Select an identity to view its fingerprint and public key.")
    );

    m_removeIdBtn = new QPushButton(tr("Remove"), left);
    m_removeIdBtn->setEnabled(false);
    leftL->addWidget(m_removeIdBtn);

    m_removeIdBtn->setToolTip(
    tr("Remove the selected identity from the local store.\n"
       "This does not delete any exported key files.")
    );

    // ===== Right: actions + editor =====
    auto *right = new QWidget(split);
    auto *rightL = new QVBoxLayout(right);
    rightL->setContentsMargins(0,0,0,0);
    rightL->setSpacing(8);

    // Action row
    auto *actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0,0,0,0);

    m_createBtn  = new QPushButton(tr("Create identity"), right);
    m_restoreBtn = new QPushButton(tr("Restore identity"), right);
    m_saveIdBtn  = new QPushButton(tr("Save identity"), right);
    m_deriveBtn  = new QPushButton(tr("Derive"), right);

    m_createBtn->setToolTip(
    tr("Create a new identity by generating a random 24-word recovery phrase.\n"
       "Write the words down and keep them safe.")
    );

    m_restoreBtn->setToolTip(
        tr("Restore an existing identity by entering your 24-word recovery phrase.")
    );

    m_deriveBtn->setToolTip(
        tr("Derive the identity fingerprint and SSH key from the entered words.")
    );

    m_saveIdBtn->setToolTip(
        tr("Save this identity to the local identity store.\n"
           "Only public information is saved (no recovery words).")
    );

    actionRow->addWidget(m_createBtn);
    actionRow->addWidget(m_restoreBtn);
    actionRow->addWidget(m_saveIdBtn);
    actionRow->addWidget(m_deriveBtn);
    actionRow->addStretch(1);

    rightL->addLayout(actionRow);

    // Form
    auto *f = new QFormLayout();
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_alias = new QLineEdit(right);
    m_words = new QPlainTextEdit(right);
    m_words->setPlaceholderText(tr("word1 word2 ... word24"));

    m_pass = new QLineEdit(right);
    m_pass->setEchoMode(QLineEdit::Password);

    m_comment = new QLineEdit(QStringLiteral("pq-ssh"), right);

    f->addRow(tr("Alias:"), m_alias);
    f->addRow(tr("24 words:"), m_words);
    f->addRow(tr("Passphrase:"), m_pass);
    f->addRow(tr("Comment:"), m_comment);

    rightL->addLayout(f);

    m_fp = new QLabel(tr("Fingerprint:"), right);
    // Prevent layout resize when text grows
    m_fp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fp->setMinimumHeight(m_fp->sizeHint().height());
    m_fp->setWordWrap(false);
    m_fp->setTextInteractionFlags(Qt::TextSelectableByMouse);

    rightL->addWidget(m_fp);

    m_pubOut = new QPlainTextEdit(right);
    m_pubOut->setReadOnly(true);
    // Prevent sudden layout jumps
    m_pubOut->setMinimumHeight(120);
    m_pubOut->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rightL->addWidget(m_pubOut, 1);

    m_words->setToolTip(
    tr("Enter your 24-word recovery phrase.\n"
       "Words can be separated by spaces or new lines.")
    );

    m_pass->setToolTip(
        tr("Optional passphrase used together with the recovery words.")
    );

    m_alias->setToolTip(
        tr("Optional human-readable name for this identity.")
    );

    m_comment->setToolTip(
        tr("Comment added to the generated SSH public key.")
    );


    // Bottom row (your existing Copy/Save buttons)
    auto *row = new QHBoxLayout();
    auto *cp = new QPushButton(tr("Copy public"));
    auto *cf = new QPushButton(tr("Copy fingerprint"));
    auto *sp = new QPushButton(tr("Save private…"));
    auto *su = new QPushButton(tr("Save public…"));
    cp->setToolTip(
        tr("Copy the OpenSSH public key to the clipboard.")
    );

    cf->setToolTip(
        tr("Copy the full identity fingerprint to the clipboard.")
    );

    sp->setToolTip(
        tr("Save the private SSH key to a file.\n"
           "Protect this file and set correct permissions.")
    );

    su->setToolTip(
        tr("Save the public SSH key to a file.")
    );

    connect(cp, &QPushButton::clicked, this, &IdentityManagerDialog::onCopyPublic);
    connect(cf, &QPushButton::clicked, this, &IdentityManagerDialog::onCopyFingerprint);
    connect(sp, &QPushButton::clicked, this, &IdentityManagerDialog::onSavePrivate);
    connect(su, &QPushButton::clicked, this, &IdentityManagerDialog::onSavePublic);

    row->addWidget(cp);
    row->addWidget(cf);
    row->addWidget(sp);
    row->addWidget(su);
    row->addStretch(1);
    rightL->addLayout(row);

    split->addWidget(left);
    split->addWidget(right);

    // Make splitter respect fixed left width and avoid jitter
    split->setCollapsible(0, false);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);

    // Set initial sizes (left, right)
    split->setSizes({320, 1200});

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    // Wiring
    connect(m_deriveBtn, &QPushButton::clicked, this, &IdentityManagerDialog::onDerive);
    connect(m_createBtn, &QPushButton::clicked, this, &IdentityManagerDialog::onCreateIdentity);
    connect(m_restoreBtn, &QPushButton::clicked, this, &IdentityManagerDialog::onRestoreIdentity);
    connect(m_saveIdBtn, &QPushButton::clicked, this, &IdentityManagerDialog::onSaveIdentity);
    connect(m_removeIdBtn, &QPushButton::clicked, this, &IdentityManagerDialog::onRemoveIdentity);
    connect(m_savedList, &QListWidget::itemClicked, this, &IdentityManagerDialog::onSelectSaved);
}


void IdentityManagerDialog::onCopyPublic()
{
    if (m_pubOut)
        QApplication::clipboard()->setText(m_pubOut->toPlainText());
}

void IdentityManagerDialog::onCopyFingerprint()
{
    QApplication::clipboard()->setText(m_fullFingerprint);
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

    m_fullFingerprint = r.fingerprintHex128;
    updateFingerprintUi();

    // NEW: track the currently derived identity so Save/Remove knows what is “current”
    m_selectedFp = r.fingerprintHex128;
    if (m_removeIdBtn) m_removeIdBtn->setEnabled(!m_selectedFp.isEmpty());

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

void IdentityManagerDialog::clearDerivedUi()
{
    m_selectedFp.clear();
    if (m_fp) m_fp->setText(tr("Fingerprint:"));
    if (m_pubOut) m_pubOut->clear();
    m_pub32.clear();
    m_priv64.clear();
    m_privFile.clear();
    if (m_removeIdBtn) m_removeIdBtn->setEnabled(false);
}
QString IdentityManagerDialog::normalizeWords(const QString &in) const
{
    QString s = in;
    s.replace("\r", " ");
    s.replace("\n", " ");
    return s.simplified();
}
QStringList IdentityManagerDialog::parseWords24(const QString &in, QString *err) const
{
    if (err) err->clear();
    const QString s = normalizeWords(in);
    if (s.isEmpty()) { if (err) *err = tr("Words are empty."); return {}; }
    const QStringList parts = s.split(' ', Qt::SkipEmptyParts);
    if (parts.size() != 24) { if (err) *err = tr("Expected 24 words, got %1.").arg(parts.size()); return {}; }
    return parts;
}
QStringList IdentityManagerDialog::loadWordlist(QString *err) const
{
    if (err) err->clear();

    // Recommended: ship a BIP39 list as a QRC resource file.
    // Example: resources/wordlists/bip39_english.txt  (2048 lines)
    QFile f(":/wordlists/bip39_english.txt");
    if (f.open(QIODevice::ReadOnly)) {
        QStringList wl;
        while (!f.atEnd()) {
            const QString w = QString::fromUtf8(f.readLine()).trimmed();
            if (!w.isEmpty()) wl << w;
        }
        if (wl.size() >= 2048) return wl;
        if (err) *err = tr("Wordlist loaded but seems too small (%1).").arg(wl.size());
        return wl;
    }

    // Fallback list (works but NOT a real mnemonic scheme!)
    return QStringList{
        "apple","binary","cable","drift","eagle","fabric","giant","hazard","icon","jungle","kitten","laser",
        "magic","native","orbit","pilot","quantum","rocket","silent","tactic","unique","vivid","window","zebra"
    };
}
QStringList IdentityManagerDialog::generateRandom24(QString *err) const
{
    if (err) err->clear();
    QString wlErr;
    const QStringList wl = loadWordlist(&wlErr);
    if (wl.size() < 24) {
        if (err) *err = wlErr.isEmpty() ? tr("Wordlist is too small.") : wlErr;
        return {};
    }

    QStringList out;
    out.reserve(24);
    for (int i = 0; i < 24; ++i) {
        const int idx = QRandomGenerator::global()->bounded(wl.size());
        out << wl.at(idx);
    }
    return out;
}
void IdentityManagerDialog::onCreateIdentity()
{
    QString err;
    const QStringList words = generateRandom24(&err);
    if (words.isEmpty()) {
        QMessageBox::warning(this, tr("Create identity"), err.isEmpty() ? tr("Failed to generate words.") : err);
        return;
    }

    // Make it readable: one per line
    if (m_words) m_words->setPlainText(words.join("\n"));

    if (m_alias) m_alias->clear();
    if (m_savedList) m_savedList->clearSelection();
    clearDerivedUi();
    if (m_alias) m_alias->setFocus();
}
void IdentityManagerDialog::onRestoreIdentity()
{
    // “Restore” just means user enters/pastes 24 words like today
    if (m_savedList) m_savedList->clearSelection();
    clearDerivedUi();
    if (m_words) m_words->setFocus();
}
QString IdentityManagerDialog::identitiesPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QDir::separator() + "identities.json";
}

bool IdentityManagerDialog::readIdentitiesJson(QJsonObject *root, QString *err) const
{
    if (err) err->clear();
    if (root) root->remove(QString());

    QFile f(identitiesPath());
    if (!f.exists()) {
        if (root) *root = QJsonObject{{"version", 1}, {"items", QJsonArray{}}};
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        if (err) *err = tr("Invalid identities.json");
        return false;
    }
    if (root) *root = doc.object();
    if (!root->contains("items") || !(*root)["items"].isArray())
        (*root)["items"] = QJsonArray{};
    if (!root->contains("version"))
        (*root)["version"] = 1;
    return true;
}

bool IdentityManagerDialog::writeIdentitiesJson(const QJsonObject &root, QString *err) const
{
    if (err) err->clear();
    QFile f(identitiesPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = f.errorString();
        return false;
    }
    const QJsonDocument doc(root);
    f.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

void IdentityManagerDialog::loadSaved()
{
    if (!m_savedList) return;
    m_savedList->clear();

    QJsonObject root;
    QString err;
    if (!readIdentitiesJson(&root, &err)) {
        QMessageBox::warning(this, tr("Identity"), tr("Cannot load identities:\n%1").arg(err));
        return;
    }

    const QJsonArray items = root["items"].toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        const QString fp = o["fingerprint"].toString();
        const QString alias = o["alias"].toString();
        const QString label = alias.isEmpty() ? fp.left(16) : alias;

        auto *it = new QListWidgetItem(label, m_savedList);
        it->setData(Qt::UserRole, fp);
        it->setToolTip(tr("Fingerprint: %1").arg(fp));
    }

    m_removeIdBtn->setEnabled(!m_selectedFp.isEmpty());
}

void IdentityManagerDialog::onSaveIdentity()
{
    // Must have a derived identity to save safely (fingerprint + pub line)
    const QString fpText = m_fp ? m_fp->text() : QString();
    const QString pubLine = m_pubOut ? m_pubOut->toPlainText().trimmed() : QString();

    if (!fpText.startsWith(tr("Fingerprint: "))) {
        QMessageBox::warning(this, tr("Save identity"), tr("Derive identity first."));
        return;
    }
    const QString fp = fpText.mid(tr("Fingerprint: ").size()).trimmed();
    if (fp.isEmpty() || pubLine.isEmpty()) {
        QMessageBox::warning(this, tr("Save identity"), tr("Derive identity first."));
        return;
    }

    QJsonObject root;
    QString err;
    if (!readIdentitiesJson(&root, &err)) {
        QMessageBox::warning(this, tr("Save identity"), err);
        return;
    }

    QJsonArray items = root["items"].toArray();

    // Upsert by fingerprint
    QJsonArray out;


    const QString alias = m_alias ? m_alias->text().trimmed() : QString();
    const QString comment = m_comment ? m_comment->text().trimmed() : QString();

    bool replaced = false;
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        if (o["fingerprint"].toString() == fp) {
            QJsonObject n = o;
            n["fingerprint"] = fp;
            n["alias"] = alias;
            n["comment"] = comment;
            n["pub"] = pubLine;
            if (!n.contains("created"))
                n["created"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            n["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            out.append(n);
            replaced = true;
        } else {
            out.append(o);
        }
    }

    if (!replaced) {
        QJsonObject n;
        n["fingerprint"] = fp;
        n["alias"] = alias;
        n["comment"] = comment;
        n["pub"] = pubLine;
        n["created"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        out.append(n);
    }

    root["items"] = out;

    if (!writeIdentitiesJson(root, &err)) {
        QMessageBox::warning(this, tr("Save identity"), err);
        return;
    }

    m_selectedFp = fp;
    loadSaved();
    QMessageBox::information(this, tr("Save identity"), tr("Identity saved."));
}

void IdentityManagerDialog::onRemoveIdentity()
{
    if (m_selectedFp.isEmpty()) return;

    const auto answer = QMessageBox::question(
        this,
        tr("Remove identity"),
        tr("Remove selected identity?\n\nThis does not delete exported key files."),
        QMessageBox::Yes | QMessageBox::No
    );
    if (answer != QMessageBox::Yes) return;

    QJsonObject root;
    QString err;
    if (!readIdentitiesJson(&root, &err)) {
        QMessageBox::warning(this, tr("Remove identity"), err);
        return;
    }

    const QJsonArray items = root["items"].toArray();
    QJsonArray out;
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        if (o["fingerprint"].toString() != m_selectedFp)
            out.append(o);
    }
    root["items"] = out;

    if (!writeIdentitiesJson(root, &err)) {
        QMessageBox::warning(this, tr("Remove identity"), err);
        return;
    }

    m_selectedFp.clear();
    clearDerivedUi();
    loadSaved();
}
void IdentityManagerDialog::onSelectSaved(QListWidgetItem *item)
{
    if (!item) return;
    const QString fp = item->data(Qt::UserRole).toString();
    if (fp.isEmpty()) return;

    QJsonObject root;
    QString err;
    if (!readIdentitiesJson(&root, &err)) {
        QMessageBox::warning(this, tr("Identity"), err);
        return;
    }

    const QJsonArray items = root["items"].toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        if (o["fingerprint"].toString() == fp) {
            m_selectedFp = fp;
            if (m_alias) m_alias->setText(o["alias"].toString());
            if (m_comment) m_comment->setText(o["comment"].toString());
            if (m_fp) m_fp->setText(tr("Fingerprint: %1").arg(fp));
            if (m_pubOut) m_pubOut->setPlainText(o["pub"].toString());

            // Don’t reveal words. User can re-enter to re-derive private key.
            if (m_words) m_words->clear();
            if (m_pass) m_pass->clear();

            if (m_removeIdBtn) m_removeIdBtn->setEnabled(true);
            return;
        }
    }
}

void IdentityManagerDialog::updateFingerprintUi()
{
    if (!m_fp) return;

    const QString prefix = tr("Fingerprint: ");
    const QString full = m_fullFingerprint.trimmed();

    if (full.isEmpty()) {
        m_fp->setText(prefix);
        m_fp->setToolTip(QString());
        return;
    }

    const int avail = qMax(0, m_fp->width()
        - m_fp->fontMetrics().horizontalAdvance(prefix) - 8);

    const QString elided = m_fp->fontMetrics().elidedText(full, Qt::ElideMiddle, avail);

    m_fp->setText(prefix + elided);
    m_fp->setToolTip(prefix + full);
}

void IdentityManagerDialog::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    updateFingerprintUi();
}
