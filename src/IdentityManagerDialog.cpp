// IdentityManagerDialog.cpp
//
// Comments focus on correctness, security hygiene, UX stability, and a few i18n gotchas.
//
// Big-picture:
// - UI/UX layout work looks solid (stable splitter, tooltips, selectable fingerprint).
// - Derivation flow is mostly OK, but there are a couple of *important* cryptographic / mnemonic issues:
//   (1) generateRandom24() is NOT BIP39-compliant as written (random words != valid mnemonic checksum).
//   (2) deriveFromWords() currently does not validate that words are from the BIP39 list / checksum.
//   If you want “real” mnemonic semantics, you need validation + proper mnemonic generation.
// - There are some smaller correctness issues around i18n string parsing, wiping secrets, and enabling Remove.
//

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

// COMMENT: bestEffortZero is fine as a *best effort*, but:
// - memset may be optimized away in some builds (though less likely for Qt QByteArray data)
// - b.clear() will just reset size; it does not guarantee underlying buffer is freed/overwritten.
// If you care: use a secure_memzero helper (like you wrote in DnaIdentityDerivation.cpp) and
// keep using it consistently.
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

    const int leftW = 320;                 // COMMENT: fixed left width is good UX for list panes.
    left->setMinimumWidth(leftW);
    left->setMaximumWidth(leftW);
    left->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    // COMMENT: You repeat split config a few times below (setCollapsible / setStretchFactor / setSizes).
    // It works, but you can remove duplicates to make maintenance easier.
    split->setCollapsible(0, false);
    split->setSizes({320, 1200});

    split->setHandleWidth(1);              // optional: thinner divider
    split->setChildrenCollapsible(false);  // prevents auto-collapse behavior

    auto *leftL = new QVBoxLayout(left);
    leftL->setContentsMargins(0,0,0,0);
    leftL->setSpacing(6);

    auto *leftTitle = new QLabel(tr("Saved identities"), left);
    leftTitle->setStyleSheet("font-weight:600;"); // COMMENT: styleSheet ok, but consider class/style role.
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

    // COMMENT: Tooltips are great here. Keep them short-ish; QTextBrowser might wrap oddly.
    m_createBtn->setToolTip(
        tr("Create a new identity by generating a random 24-word recovery phrase.\n"
           "Write the words down and keep them safe.")
    );

    // IMPORTANT COMMENT: As implemented later, "random 24 words" is NOT a valid BIP39 mnemonic.
    // BIP39 mnemonics require entropy+checksum mapping into word indices.
    // If you keep this UX, consider rewording:
    //   "Generate a 24-word phrase from the BIP39 wordlist (not checksum-validated yet)."
    // Or implement real mnemonic generation.

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

    m_comment = new QLineEdit(QStringLiteral("pq-ssh"), right); // COMMENT: ok default

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

    // COMMENT: tooltip coverage is good. Consider also tooltip on fingerprint label.
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

    // Bottom row (Copy/Save buttons)
    auto *row = new QHBoxLayout();
    auto *cp = new QPushButton(tr("Copy public"));
    auto *cf = new QPushButton(tr("Copy fingerprint"));
    auto *sp = new QPushButton(tr("Save private…"));
    auto *su = new QPushButton(tr("Save public…"));

    // COMMENT: Copy buttons should probably be disabled until derived/saved content exists.
    // Not required, but prevents copying empty strings.

    cp->setToolTip(tr("Copy the OpenSSH public key to the clipboard."));
    cf->setToolTip(tr("Copy the full identity fingerprint to the clipboard."));
    sp->setToolTip(tr("Save the private SSH key to a file.\nProtect this file and set correct permissions."));
    su->setToolTip(tr("Save the public SSH key to a file."));

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

    // COMMENT: duplicate stretchFactor calls above; safe but remove duplicates.

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
    // COMMENT: If empty, you might want to no-op or show a gentle message.
    if (m_pubOut)
        QApplication::clipboard()->setText(m_pubOut->toPlainText());
}

void IdentityManagerDialog::onCopyFingerprint()
{
    // COMMENT: If m_fullFingerprint empty, clipboard gets empty. Consider guarding.
    QApplication::clipboard()->setText(m_fullFingerprint.trimmed());
}

void IdentityManagerDialog::onSavePrivate()
{
    if (m_privFile.isEmpty()) {
        QMessageBox::warning(this, tr("Save private key"), tr("No private key derived yet."));
        return;
    }

    // COMMENT: Provide a better default path:
    // QStandardPaths::writableLocation(DocumentsLocation) or last-used dir.
    const QString p = QFileDialog::getSaveFileName(this, tr("Save private key"), tr("id_ed25519"));
    if (p.isEmpty()) return;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, tr("Save private key"), tr("Cannot write file:\n%1").arg(f.errorString()));
        return;
    }

    const qint64 written = f.write(m_privFile);
    f.close();

    // COMMENT: On Linux, also consider setting permissions *before* writing
    // via QSaveFile or umask-like behavior. What you have is usually fine.
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
    const QString wordsRaw = m_words ? m_words->toPlainText() : QString();
    const QString pass     = m_pass  ? m_pass->text()         : QString();

    // ✅ Minimal validation: exactly 24 words (real BIP39 checksum validation optional/future)
    QString parseErr;
    const QStringList w24 = parseWords24(wordsRaw, &parseErr);
    if (w24.isEmpty()) {
        QMessageBox::warning(this, tr("Identity"), parseErr);
        return;
    }

    // Use a stable normalized representation for derivation
    const QString words = w24.join(' ');

    // 1) Derive DNA identity (PQ fingerprint, etc.)
    const auto r = DnaIdentityDerivation::deriveFromWords(words, pass);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Identity"), tr("Failed: %1").arg(r.error));
        return;
    }

    m_fullFingerprint = r.fingerprintHex128;
    updateFingerprintUi();

    // ✅ IMPORTANT FIX:
    // Do NOT set m_selectedFp here and do NOT enable "Remove" for a freshly derived (unsaved) identity.
    // Remove should be enabled only when a saved identity is selected from the list.
    if (m_removeIdBtn) m_removeIdBtn->setEnabled(!m_selectedFp.isEmpty());

    // 2) Derive Ed25519 SSH key (seed32 = SHAKE256(master64 || ctx))
    QByteArray master64 = DnaIdentityDerivation::bip39Seed64(words, pass);

    // Make sure we wipe temporaries on ALL exit paths
    auto wipeTemps = [&]() {
        bestEffortZero(master64);
        // 'in' and 'edSeed32' are wiped below where they exist
    };

    if (master64.size() != 64) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to derive BIP39 master seed."));
        wipeTemps();
        return;
    }

    const QByteArray ctx = QByteArrayLiteral("cpunk-pqssh-ed25519-v1"); // must stay stable forever
    QByteArray in = master64 + ctx;

    QByteArray edSeed32 = DnaIdentityDerivation::shake256_32(in);
    if (edSeed32.size() != 32) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to derive Ed25519 seed (SHAKE256)."));
        bestEffortZero(in);
        bestEffortZero(edSeed32);
        wipeTemps();
        return;
    }

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(edSeed32.constData()),
        static_cast<size_t>(edSeed32.size())
    );
    if (!pkey) {
        QMessageBox::critical(this, tr("Identity"), tr("Failed to create Ed25519 key."));
        bestEffortZero(in);
        bestEffortZero(edSeed32);
        wipeTemps();
        return;
    }

    unsigned char pub[32];
    size_t pubLen = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &pubLen) != 1 || pubLen != 32) {
        EVP_PKEY_free(pkey);
        QMessageBox::critical(this, tr("Identity"), tr("Failed to extract Ed25519 public key."));
        bestEffortZero(in);
        bestEffortZero(edSeed32);
        wipeTemps();
        return;
    }
    EVP_PKEY_free(pkey);

    m_pub32  = QByteArray(reinterpret_cast<const char*>(pub), 32);
    m_priv64 = edSeed32 + m_pub32;

    const QString comment = m_comment ? m_comment->text() : QStringLiteral("pq-ssh");
    if (m_pubOut)
        m_pubOut->setPlainText(OpenSshEd25519Key::publicKeyLine(m_pub32, comment));
    m_privFile = OpenSshEd25519Key::privateKeyFile(m_pub32, m_priv64, comment);

    // wipe sensitive temps
    bestEffortZero(in);
    bestEffortZero(edSeed32);
    wipeTemps();
}


void IdentityManagerDialog::clearDerivedUi()
{
    // COMMENT: Good to clear derived outputs; but you may want to keep m_selectedFp if user selected
    // a saved identity (so Remove stays available) vs "clear derived" for restore/create actions.
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
    // COMMENT: ok; you later call simplified() which collapses whitespace anyway.
    QString s = in;
    s.replace("\r", " ");
    s.replace("\n", " ");
    return s.simplified();
}

QStringList IdentityManagerDialog::parseWords24(const QString &in, QString *err) const
{
    // COMMENT: This is good validation, but currently unused in onDerive().
    // Strongly recommend using it to guard obvious mistakes.
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

    // COMMENT: Good: ship BIP39 list as QRC.
    // But note: BIP39 requires exact 2048 words; you check >=2048, which is fine.
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

    // IMPORTANT COMMENT:
    // This fallback list is *not* BIP39 and will generate mnemonics that are incompatible
    // with any external wallet/tool. It’s okay for dev/testing, but for production:
    // - remove fallback, or
    // - clearly label as “demo wordlist”.
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

    // IMPORTANT COMMENT:
    // Picking 24 random words from the BIP39 list is NOT a valid BIP39 mnemonic
    // (checksum will almost certainly be wrong).
    // If you don’t care about BIP39 compatibility, you should rename UI strings from “BIP39”
    // and/or document that this is a “wordlist-based passphrase”.
    //
    // If you DO care, implement real mnemonic generation:
    // - Generate 256-bit entropy
    // - Append checksum bits (8 bits)
    // - Split into 24 x 11-bit indices -> map into wordlist
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
    const QString mnemonic =
        DnaIdentityDerivation::generateBip39Mnemonic24(&err);

    if (mnemonic.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Create identity"),
            err.isEmpty()
                ? tr("Failed to generate BIP39 recovery phrase.")
                : err
        );
        return;
    }

    // Display as one word per line (UX-friendly)
    if (m_words)
        m_words->setPlainText(mnemonic.split(' ').join('\n'));

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
    // COMMENT: AppDataLocation is fine. On Linux it maps to ~/.local/share/<org>/<app>
    // (depending on your QCoreApplication org/app name).
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QDir::separator() + "identities.json";
}

bool IdentityManagerDialog::readIdentitiesJson(QJsonObject *root, QString *err) const
{
    // COMMENT: root->remove(QString()) is unusual; it removes the key "" only.
    // If intention is "clear object", just do: if (root) *root = QJsonObject{};
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

    // COMMENT: This assumes `root` is non-null below; either assert or guard.
    if (!root->contains("items") || !(*root)["items"].isArray())
        (*root)["items"] = QJsonArray{};
    if (!root->contains("version"))
        (*root)["version"] = 1;
    return true;
}

bool IdentityManagerDialog::writeIdentitiesJson(const QJsonObject &root, QString *err) const
{
    // COMMENT: Consider QSaveFile to avoid corrupting identities.json on crash/power loss.
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

        // COMMENT: showing fp.left(16) is fine; tooltip includes full.
        const QString label = alias.isEmpty() ? fp.left(16) : alias;

        auto *it = new QListWidgetItem(label, m_savedList);
        it->setData(Qt::UserRole, fp);
        it->setToolTip(tr("Fingerprint: %1").arg(fp));
    }

    // COMMENT: This enables Remove based on m_selectedFp (which might refer to last derived identity).
    // Better logic: enable Remove ONLY if the currently selected list item exists.
    // For example:
    //   m_removeIdBtn->setEnabled(m_savedList->currentItem() != nullptr);
    m_removeIdBtn->setEnabled(!m_selectedFp.isEmpty());
}


void IdentityManagerDialog::onSaveIdentity()
{
    // Must have a derived identity to save safely (fingerprint + pub line)
    const QString fp = m_fullFingerprint.trimmed();
    const QString pubLine = m_pubOut ? m_pubOut->toPlainText().trimmed() : QString();

    // IMPORTANT (i18n + elide bug):
    // Do NOT parse m_fp->text() to extract fingerprint.
    // - "Fingerprint:" is translated (startsWith(...) breaks across languages)
    // - updateFingerprintUi() elides the fingerprint for display (abcd…wxyz),
    //   so the label text is not the full value anyway.
    //
    // Source of truth is m_fullFingerprint (set on Derive / Select Saved).
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

    const QString alias   = m_alias   ? m_alias->text().trimmed()   : QString();
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

            // created/updated timestamps are good.
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

    // Mark the saved identity as "selected/current"
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

            // ✅ Source of truth for Copy/Save flows
            m_fullFingerprint = fp;
            updateFingerprintUi(); // keeps label elided/stable and tooltip full

            // Good UX: selecting saved identity does NOT restore secret words.
            if (m_alias)   m_alias->setText(o["alias"].toString());
            if (m_comment) m_comment->setText(o["comment"].toString());

            // Prefer the elided UI helper rather than setting label text directly:
            // if (m_fp) m_fp->setText(tr("Fingerprint: %1").arg(fp));

            if (m_pubOut)  m_pubOut->setPlainText(o["pub"].toString());

            if (m_words) m_words->clear();
            if (m_pass)  m_pass->clear();

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

    // COMMENT: Nice: elide middle and put full value into tooltip.
    // But note: onSaveIdentity() currently reads m_fp->text(), which may be elided.
    // So onSaveIdentity() must be changed to use m_fullFingerprint.
    const QString elided = m_fp->fontMetrics().elidedText(full, Qt::ElideMiddle, avail);

    m_fp->setText(prefix + elided);
    m_fp->setToolTip(prefix + full);
}

void IdentityManagerDialog::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    updateFingerprintUi();
}
