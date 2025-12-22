// KeyGeneratorDialog.cpp
//
// Purpose:
//   GUI for generating, inventorying, and managing SSH keys used by pq-ssh.
//
// Responsibilities:
//   - Generate keys (OpenSSH ed25519 / RSA, plus Dilithium5 stub)
//   - Encrypt Dilithium private keys at rest using libsodium
//   - Maintain metadata.json describing keys (labels, owner, expiry, status)
//   - Provide a searchable/sortable key inventory UI
//   - Export / copy public keys
//   - Emit signal to install selected public key onto a remote server
//
// Non-goals (by design):
//   - No direct SSH connections here
//   - No server-side logic
//   - No DNA-identity concepts (explicitly excluded for now)
//
// Security model:
//   - Private keys live under ~/.pq-ssh/keys
//   - Dilithium private keys are *always encrypted at rest*
//   - OpenSSH keys may be encrypted or plaintext (detected automatically)
//   - Passphrases are never logged or persisted
//
#include "KeyGeneratorDialog.h"
#include "KeyMetadataUtils.h"
#include "DilithiumKeyCrypto.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QDateTime>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QColor>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QBrush>
#include <QGroupBox>
#include <sodium.h>
#include <QMenu>
#include <QInputDialog>

// Return current UTC timestamp in ISO 8601 with milliseconds.
// Used consistently across metadata.json.
static QString isoUtcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// Detect whether an OpenSSH private key is encrypted.
//
// Used purely for UI signaling (🔒 icon),
// not for unlocking or decryption.
static bool isOpenSshPrivateKeyEncrypted(const QString &privPath);

// ---------------------------------------------------------------------------
// libsodium init + base64 helpers
// ---------------------------------------------------------------------------
// libsodium must be initialized exactly once per process.
// This helper enforces lazy, idempotent initialization.
static bool sodiumInitOnce(QString *errOut)
{
    static bool inited = false;
    if (inited) return true;

    if (sodium_init() < 0) {
        if (errOut) *errOut = QObject::tr("libsodium init failed (sodium_init).");
        return false;
    }
    inited = true;
    return true;
}

// Base64 without '=' padding.
// Matches OpenSSH fingerprint style and avoids visual clutter.
static QByteArray b64NoPad(const QByteArray &in)
{
    QByteArray b = in.toBase64();
    while (!b.isEmpty() && b.endsWith('=')) b.chop(1);
    return b;
}

// Decode base64 while tolerating missing padding.
// Useful when parsing public key lines.
static QByteArray b64DecodeLoose(const QByteArray &in)
{
    QByteArray b = in.trimmed();
    const int mod = b.size() % 4;
    if (mod) b.append(QByteArray(4 - mod, '='));
    return QByteArray::fromBase64(b);
}

// ---------------------------------------------------------------------------
// PQSSH helpers (Dilithium5 + fingerprints)
// ---------------------------------------------------------------------------
// Compute SHA256 fingerprint and format it as:
//   SHA256:<base64-no-padding>
//
// This matches ssh-keygen -lf -E sha256 output format.
static QString sha256Fingerprint(const QByteArray &data)
{
    const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    QString b64 = QString::fromLatin1(digest.toBase64());
    b64.remove('=');
    return "SHA256:" + b64;
}

// Temporary stub generator for Dilithium5 keys.
//
// IMPORTANT:
//   This does NOT implement real Dilithium cryptography.
//   It exists solely to validate UI flow, encryption-at-rest,
//   metadata handling, and install mechanics.
//
// Future:
//   Replace with real PQ keygen (liboqs / reference impl).
static bool runDilithium5StubKeygen(const QString &privPath,
                                   const QString &comment,
                                   const QString &passphrase,
                                   QString *errOut)
{
    Q_UNUSED(passphrase);

    if (!sodiumInitOnce(errOut)) return false;

    QByteArray pub(2592, Qt::Uninitialized);
    QByteArray priv(4896, Qt::Uninitialized);

    randombytes_buf(pub.data(),  (size_t)pub.size());
    randombytes_buf(priv.data(), (size_t)priv.size());

    QFile::remove(privPath);
    QFile::remove(privPath + ".pub");

    {
        QFile f(privPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errOut) *errOut = QObject::tr("Failed to write private key: %1").arg(f.errorString());
            return false;
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(priv);
        f.close();
    }

    {
        QFile f(privPath + ".pub");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errOut) *errOut = QObject::tr("Failed to write public key: %1").arg(f.errorString());
            return false;
        }

        QByteArray line = "pqssh-dilithium5 ";
        line += b64NoPad(pub);
        if (!comment.isEmpty()) {
            line += " ";
            line += comment.toUtf8();
        }
        line += "\n";
        f.write(line);
        f.close();
    }

    return true;
}

// ============================================================================
// Inline helper dialog (must be defined BEFORE use in onEditMetadata())
// ============================================================================
// Lightweight modal dialog for editing *existing* key metadata.
//
// Embedded here intentionally:
//   - Only used by KeyGeneratorDialog
//   - Keeps metadata UX logic close to inventory logic
//   - Avoids over-abstracting a UI that may change often
class EditMetadataDialog : public QDialog
{
public:
    explicit EditMetadataDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle(tr("Edit Key Metadata"));
        setModal(true);
        resize(520, 260);

        auto *root = new QVBoxLayout(this);
        auto *form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignRight);

        labelEdit = new QLineEdit(this);
        ownerEdit = new QLineEdit(this);
        purposeEdit = new QLineEdit(this);

        rotationSpin = new QSpinBox(this);
        rotationSpin->setRange(0, 3650);

        auto *expireRow = new QWidget(this);
        auto *expireLay = new QHBoxLayout(expireRow);
        expireLay->setContentsMargins(0,0,0,0);

        expireCheck = new QCheckBox(tr("Enable"), this);
        expireDate = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addDays(90), this);
        expireDate->setDisplayFormat("yyyy-MM-dd HH:mm 'UTC'");
        expireDate->setTimeSpec(Qt::UTC);
        expireDate->setEnabled(false);

        QObject::connect(expireCheck, &QCheckBox::toggled, expireDate, &QWidget::setEnabled);

        expireLay->addWidget(expireCheck);
        expireLay->addWidget(expireDate, 1);

        form->addRow(tr("Label:"), labelEdit);
        form->addRow(tr("Owner:"), ownerEdit);
        form->addRow(tr("Purpose:"), purposeEdit);
        form->addRow(tr("Rotation policy (days):"), rotationSpin);
        form->addRow(tr("Expire date:"), expireRow);

        root->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
    }

    void setInitial(const QString &label,
                    const QString &owner,
                    const QString &purpose,
                    int rotationDays,
                    const QString &expiresIsoOrEmpty)
    {
        labelEdit->setText(label);
        ownerEdit->setText(owner);
        purposeEdit->setText(purpose);
        rotationSpin->setValue(rotationDays);

        if (!expiresIsoOrEmpty.isEmpty()) {
            expireCheck->setChecked(true);
            const auto dt = QDateTime::fromString(expiresIsoOrEmpty, Qt::ISODateWithMs);
            if (dt.isValid())
                expireDate->setDateTime(dt.toUTC());
        } else {
            expireCheck->setChecked(false);
        }
    }

    QString label() const { return labelEdit->text().trimmed(); }
    QString owner() const { return ownerEdit->text().trimmed(); }
    QString purpose() const { return purposeEdit->text().trimmed(); }
    int rotationDays() const { return rotationSpin->value(); }

    QString expiresIsoOrEmpty() const
    {
        if (!expireCheck->isChecked()) return QString();
        return expireDate->dateTime().toUTC().toString(Qt::ISODateWithMs);
    }

private:
    QLineEdit *labelEdit{};
    QLineEdit *ownerEdit{};
    QLineEdit *purposeEdit{};
    QSpinBox *rotationSpin{};
    QCheckBox *expireCheck{};
    QDateTimeEdit *expireDate{};
};

// ============================================================================
// KeyGeneratorDialog
// ============================================================================
// Main dialog constructor.
//
// UI structure:
//   Tabs:
//     1) Generate — create new keys + metadata
//     2) Keys     — inventory, status, install, revoke, delete
//
// profileNames:
//   Injected list of SSH profiles from MainWindow.
//   Used ONLY for "Install selected key".
KeyGeneratorDialog::KeyGeneratorDialog(const QStringList& profileNames, QWidget *parent)
    : QDialog(parent),
      m_profileNames(profileNames)
{
    setWindowTitle(tr("Key Generator"));
    setModal(true);
    resize(920, 520);

    auto *root = new QVBoxLayout(this);

    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs, 1);

    // ============================
    // TAB 1: Generate
    // ============================
    auto *genTab = new QWidget(this);
    auto *genLayout = new QVBoxLayout(genTab);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    m_algoCombo = new QComboBox(this);
    m_algoCombo->addItems({ "ed25519", "rsa3072", "rsa4096", "dilithium5" });
    form->addRow(tr("Algorithm:"), m_algoCombo);

    m_keyNameEdit = new QLineEdit(this);
    m_keyNameEdit->setPlaceholderText(tr("id_ed25519_pqssh"));
    form->addRow(tr("Key filename:"), m_keyNameEdit);

    m_labelEdit = new QLineEdit(this);
    form->addRow(tr("Label:"), m_labelEdit);

    m_ownerEdit = new QLineEdit(this);
    const QString envUser = qEnvironmentVariable("USER");
    if (!envUser.isEmpty()) m_ownerEdit->setText(envUser);
    form->addRow(tr("Owner:"), m_ownerEdit);

    m_purposeEdit = new QLineEdit(this);
    m_purposeEdit->setPlaceholderText(tr("e.g. admin, prod-access, ci-deployment"));
    form->addRow(tr("Purpose:"), m_purposeEdit);

    m_rotationSpin = new QSpinBox(this);
    m_rotationSpin->setRange(0, 3650);
    m_rotationSpin->setValue(90);
    form->addRow(tr("Rotation policy (days):"), m_rotationSpin);

    m_statusCombo = new QComboBox(this);
    m_statusCombo->addItems({ "active", "revoked", "expired" });
    m_statusCombo->setCurrentText("active");
    form->addRow(tr("Status:"), m_statusCombo);

    auto *expireRow = new QWidget(this);
    auto *expireLayout = new QHBoxLayout(expireRow);
    expireLayout->setContentsMargins(0,0,0,0);

    m_expireCheck = new QCheckBox(tr("Enable"), this);
    m_expireDate = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addDays(90), this);
    m_expireDate->setDisplayFormat("yyyy-MM-dd HH:mm 'UTC'");
    m_expireDate->setTimeSpec(Qt::UTC);
    m_expireDate->setEnabled(false);

    connect(m_expireCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_expireDate->setEnabled(on);
    });

    expireLayout->addWidget(m_expireCheck);
    expireLayout->addWidget(m_expireDate, 1);
    form->addRow(tr("Expire date:"), expireRow);

    m_pass1Edit = new QLineEdit(this);
    m_pass1Edit->setEchoMode(QLineEdit::Password);
    m_pass2Edit = new QLineEdit(this);
    m_pass2Edit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Passphrase:"), m_pass1Edit);
    form->addRow(tr("Passphrase (again):"), m_pass2Edit);

    auto updatePassphraseUi = [this]() {
        const bool isDilithium = (m_algoCombo->currentText() == "dilithium5");
        m_pass1Edit->setEnabled(true);
        m_pass2Edit->setEnabled(true);

        if (isDilithium) {
            m_pass1Edit->setPlaceholderText(tr("Encrypt Dilithium5 private key (required)"));
            m_pass2Edit->setPlaceholderText(tr("Repeat passphrase"));
            m_pass1Edit->setToolTip(tr("Used to encrypt Dilithium5 private key (Argon2id + XChaCha20-Poly1305)."));
            m_pass2Edit->setToolTip(tr("Used to encrypt Dilithium5 private key (Argon2id + XChaCha20-Poly1305)."));
        } else {
            m_pass1Edit->setPlaceholderText(QString());
            m_pass2Edit->setPlaceholderText(QString());
            m_pass1Edit->setToolTip(QString());
            m_pass2Edit->setToolTip(QString());
        }
    };
    updatePassphraseUi();
    connect(m_algoCombo, &QComboBox::currentTextChanged, this, [updatePassphraseUi](const QString&) {
        updatePassphraseUi();
    });

    genLayout->addLayout(form);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setText(tr("Keys are saved under:\n%1").arg(keysDir()));
    genLayout->addWidget(m_resultLabel);

    auto *genButtonsRow = new QHBoxLayout();
    genButtonsRow->addStretch(1);
    m_generateBtn = new QPushButton(tr("Generate"), this);
    genButtonsRow->addWidget(m_generateBtn);
    genLayout->addLayout(genButtonsRow);

    connect(m_generateBtn, &QPushButton::clicked, this, &KeyGeneratorDialog::onGenerate);

    m_tabs->addTab(genTab, tr("Generate"));

    // ============================
    // TAB 2: Keys (Manage)
    // ============================
    auto *keysTab = new QWidget(this);
    auto *keysLayout = new QVBoxLayout(keysTab);

    m_keysHintLabel = new QLabel(this);
    m_keysHintLabel->setWordWrap(true);
    m_keysHintLabel->setText(tr("Inventory:\n- metadata: %1\n- key files: %2/*.pub")
                                 .arg(metadataPath(), keysDir()));
    keysLayout->addWidget(m_keysHintLabel);

    m_table = new QTableWidget(this);

    // Right-click menu on keys table (put it HERE, after m_table exists)
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &KeyGeneratorDialog::onKeysContextMenuRequested);

    m_table->setColumnCount(10);
    m_table->setHorizontalHeaderLabels({
        tr("Fingerprint"), tr("Label"), tr("Owner"), tr("Algorithm"), tr("Status"),
        tr("Created"), tr("Expires"), tr("Purpose"), tr("Rotation"), tr("Key file")
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);

    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    m_table->horizontalHeader()->setSectionsClickable(true);

    m_sortColumn = m_table->horizontalHeader()->sortIndicatorSection();
    m_sortOrder  = m_table->horizontalHeader()->sortIndicatorOrder();

    connect(m_table->horizontalHeader(), &QHeaderView::sortIndicatorChanged,
            this, [this](int col, Qt::SortOrder order) {
                m_sortColumn = col;
                m_sortOrder  = order;
            });

    m_table->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);

    keysLayout->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    m_refreshBtn   = new QPushButton(tr("Refresh"), this);
    m_copyFpBtn    = new QPushButton(tr("Copy fingerprint"), this);
    m_copyPubBtn   = new QPushButton(tr("Copy public key"), this);
    m_exportPubBtn = new QPushButton(tr("Export pubkey..."), this);

    // ✅ NEW button (your desired flow)
    m_installBtn   = new QPushButton(tr("Install selected key…"), this);
    m_installBtn->setToolTip(tr("Choose a profile and install this public key into ~/.ssh/authorized_keys on that server."));

    m_editMetaBtn  = new QPushButton(tr("Edit metadata..."), this);
    m_revokeBtn    = new QPushButton(tr("Mark revoked"), this);
    m_deleteBtn    = new QPushButton(tr("Delete"), this);
    m_deleteFilesCheck = new QCheckBox(tr("Also delete key files"), this);

    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_copyFpBtn);
    btnRow->addWidget(m_copyPubBtn);
    btnRow->addWidget(m_exportPubBtn);
    btnRow->addWidget(m_installBtn);     // ✅ placed near pubkey actions
    btnRow->addWidget(m_editMetaBtn);
    btnRow->addWidget(m_revokeBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addWidget(m_deleteFilesCheck);

    keysLayout->addLayout(btnRow);

    connect(m_refreshBtn,   &QPushButton::clicked, this, &KeyGeneratorDialog::refreshKeysTable);
    connect(m_copyFpBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onCopyFingerprint);
    connect(m_copyPubBtn,   &QPushButton::clicked, this, &KeyGeneratorDialog::onCopyPublicKey);
    connect(m_exportPubBtn, &QPushButton::clicked, this, &KeyGeneratorDialog::onExportPublicKey);
    connect(m_installBtn,   &QPushButton::clicked, this, &KeyGeneratorDialog::onInstallSelectedKey);

    connect(m_editMetaBtn,  &QPushButton::clicked, this, &KeyGeneratorDialog::onEditMetadata);
    connect(m_revokeBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onMarkRevoked);
    connect(m_deleteBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onDeleteKey);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, &KeyGeneratorDialog::onKeySelectionChanged);

    m_tabs->addTab(keysTab, tr("Keys"));

    // Bottom close
    auto *buttons = new QDialogButtonBox(this);
    auto *closeBtn = buttons->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    root->addWidget(buttons);

    refreshKeysTable();
    onKeySelectionChanged();
}

// ~/.pq-ssh/keys
//   - id_ed25519_*.pub
//   - id_ed25519_*
//   - id_dilithium5_*.enc
//   - metadata.json
// Key generation flow (onGenerate):
//
// 1. Validate input (algo, passphrase rules)
// 2. Ensure ~/.pq-ssh/keys exists
// 3. Run ssh-keygen OR Dilithium stub generator
// 4. If Dilithium:
//      - Encrypt private key using DilithiumKeyCrypto
//      - Remove plaintext private key
// 5. Compute fingerprint
// 6. Persist metadata.json entry
// 7. Refresh inventory UI
QString KeyGeneratorDialog::keysDir() const
{
    return QDir(QDir::homePath()).filePath(".pq-ssh/keys");
}

QString KeyGeneratorDialog::metadataPath() const
{
    return QDir(keysDir()).filePath("metadata.json");
}

bool KeyGeneratorDialog::ensureKeysDir(QString *errOut)
{
    QDir d(keysDir());
    if (d.exists()) return true;
    if (!d.mkpath(".")) {
        if (errOut) *errOut = tr("Failed to create keys directory: %1").arg(keysDir());
        return false;
    }
    return true;
}

bool KeyGeneratorDialog::runSshKeygen(const QString &algo, const QString &privPath,
                                     const QString &comment, const QString &passphrase,
                                     QString *errOut)
{
    if (algo == "dilithium5") {
        return runDilithium5StubKeygen(privPath, comment, passphrase, errOut);
    }

    QStringList args;

    if (algo == "ed25519") {
        args << "-t" << "ed25519";
    } else if (algo == "rsa3072") {
        args << "-t" << "rsa" << "-b" << "3072";
    } else if (algo == "rsa4096") {
        args << "-t" << "rsa" << "-b" << "4096";
    } else {
        if (errOut) *errOut = tr("Unsupported algorithm: %1").arg(algo);
        return false;
    }

    QFile::remove(privPath);
    QFile::remove(privPath + ".pub");

    args << "-f" << privPath;
    if (!comment.isEmpty()) args << "-C" << comment;
    args << "-N" << passphrase;

    QProcess p;
    p.start("ssh-keygen", args);
    if (!p.waitForFinished(30000)) {
        if (errOut) *errOut = tr("ssh-keygen timed out.");
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (errOut) *errOut = tr("ssh-keygen failed:\n%1")
            .arg(QString::fromLocal8Bit(p.readAllStandardError()));
        return false;
    }
    return true;
}

bool KeyGeneratorDialog::computeFingerprint(const QString &pubPath, QString *fpOut, QString *errOut)
{
    {
        QProcess p;
        p.start("ssh-keygen", { "-lf", pubPath, "-E", "sha256" });

        if (p.waitForFinished(15000) &&
            p.exitStatus() == QProcess::NormalExit &&
            p.exitCode() == 0)
        {
            const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
            const QStringList parts = out.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (parts.size() >= 2 && parts[1].startsWith("SHA256:")) {
                *fpOut = parts[1];
                return true;
            }
        }
    }

    QFile f(pubPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = tr("Cannot read public key: %1").arg(f.errorString());
        return false;
    }
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    f.close();

    const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        if (errOut) *errOut = tr("Unrecognized public key format:\n%1").arg(line);
        return false;
    }

    const QString kind = parts[0];
    const QByteArray b64  = parts[1].toLatin1();

    if (!kind.startsWith("pqssh-")) {
        if (errOut) *errOut = tr("Unsupported public key format (not OpenSSH, not PQSSH): %1").arg(kind);
        return false;
    }

    const QByteArray pub = b64DecodeLoose(b64);
    if (pub.isEmpty()) {
        if (errOut) *errOut = tr("Invalid base64 public key data.");
        return false;
    }

    *fpOut = sha256Fingerprint(pub);
    return true;
}

// ---------------- Metadata IO ----------------

bool KeyGeneratorDialog::loadMetadata(QMap<QString, QJsonObject> *out, QString *errOut)
{
    out->clear();

    QFile f(metadataPath());
    if (!f.exists()) return true;

    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = tr("Cannot read metadata.json");
        return false;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject()) return true;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.value().isObject())
            (*out)[it.key()] = it.value().toObject();
    }
    return true;
}

bool KeyGeneratorDialog::writeMetadata(const QJsonObject &root, QString *errOut)
{
    QFile f(metadataPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = tr("Cannot write metadata.json");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool KeyGeneratorDialog::saveMetadata(const QString &fingerprint,
                                      const QString &label,
                                      const QString &owner,
                                      const QString &createdIso,
                                      const QString &expiresIsoOrEmpty,
                                      const QString &purpose,
                                      const QString &algorithm,
                                      int rotationDays,
                                      const QString &status,
                                      const QString &privPath,
                                      const QString &pubPath,
                                      QString *errOut)
{
    QJsonObject rootObj;
    QFile f(metadataPath());
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) {
            if (errOut) *errOut = tr("Cannot read metadata.json");
            return false;
        }
        const auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (doc.isObject()) rootObj = doc.object();
    }

    QJsonObject meta;
    meta["key_fingerprint"] = fingerprint;
    meta["label"] = label;
    meta["owner"] = owner;
    meta["created_date"] = createdIso;
    meta["expire_date"] = expiresIsoOrEmpty.isEmpty()
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(expiresIsoOrEmpty);
    meta["purpose"] = purpose;
    meta["algorithm"] = algorithm;
    meta["rotation_policy_days"] = rotationDays;
    meta["status"] = status;

    meta["private_key_path"] = privPath;
    meta["public_key_path"]  = pubPath;

    rootObj[fingerprint] = meta;

    return writeMetadata(rootObj, errOut);
}

// -------------- Inventory: metadata + file scan -----------------

QString KeyGeneratorDialog::readPublicKeyLine(const QString &pubPath)
{
    QFile f(pubPath);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    f.close();
    return line;
}

// Inventory is built by *merging*:
//
//   A) Files on disk (*.pub)
//   B) metadata.json entries
//
// This allows:
//   - Detecting orphaned files
//   - Detecting missing metadata
//   - Showing warnings visually in UI
QMap<QString, KeyGeneratorDialog::KeyRow> KeyGeneratorDialog::buildInventory(QString *errOut)
{
    QMap<QString, KeyRow> inv;

    QDir d(keysDir());
    const QStringList pubs = d.entryList({ "*.pub" }, QDir::Files, QDir::Name);
    for (const QString &pubFile : pubs) {
        const QString pubPath = d.filePath(pubFile);

        const QString pubLine = readPublicKeyLine(pubPath);

        QString inferredAlgo;
        if (!pubLine.isEmpty()) {
            const QStringList parts = pubLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const QString kind = parts.isEmpty() ? QString() : parts[0];

            if (kind == "pqssh-dilithium5") inferredAlgo = "dilithium5";
            else if (kind == "ssh-ed25519") inferredAlgo = "ed25519";
            else if (kind == "ssh-rsa") inferredAlgo = "rsa";
            else if (kind.startsWith("pqssh-")) inferredAlgo = kind.mid(QString("pqssh-").size());
        }

        QString inferredCreated;
        {
            const QFileInfo fi(pubPath);
            const QDateTime mtimeUtc = fi.lastModified().toUTC();
            if (mtimeUtc.isValid())
                inferredCreated = mtimeUtc.toString(Qt::ISODateWithMs);
        }

        QString fp;
        QString e;
        if (!computeFingerprint(pubPath, &fp, &e)) {
            continue;
        }

        KeyRow row;
        row.fingerprint = fp;
        row.pubPath = pubPath;
        row.privPath = pubPath.left(pubPath.size() - 4);
        row.comment = pubLine;
        row.algorithm = inferredAlgo;
        row.created = inferredCreated;
        row.hasFiles = true;

        inv[fp] = row;
    }

    QMap<QString, QJsonObject> metaMap;
    QString metaErr;
    if (!loadMetadata(&metaMap, &metaErr)) {
        if (errOut) *errOut = metaErr;
    }

    for (auto it = metaMap.begin(); it != metaMap.end(); ++it) {
        const QString fp = it.key();
        const QJsonObject m = it.value();

        KeyRow row = inv.contains(fp) ? inv[fp] : KeyRow{};
        row.fingerprint = fp;

        row.label = m.value("label").toString();
        row.owner = m.value("owner").toString();
        row.created = m.value("created_date").toString();
        row.expires = m.value("expire_date").isString() ? m.value("expire_date").toString() : QString();
        row.purpose = m.value("purpose").toString();
        row.algorithm = m.value("algorithm").toString();
        row.rotationDays = m.value("rotation_policy_days").toInt();
        row.status = m.value("status").toString();
        row.hasMetadata = true;

        const QString mpub = m.value("public_key_path").toString();
        const QString mpriv = m.value("private_key_path").toString();
        if (!mpub.isEmpty()) row.pubPath = mpub;
        if (!mpriv.isEmpty()) row.privPath = mpriv;

        if (!row.pubPath.isEmpty() && QFile::exists(row.pubPath))
            row.hasFiles = true;

        inv[fp] = row;
    }

    return inv;
}

// ---------------- UI actions ----------------

void KeyGeneratorDialog::onGenerate()
{
    const QString algo = m_algoCombo->currentText();
    const bool isDilithium = (algo == "dilithium5");

    if (isDilithium) {
        if (m_pass1Edit->text().isEmpty()) {
            m_resultLabel->setText(tr("Dilithium5 keys require a passphrase."));
            return;
        }
        if (m_pass1Edit->text() != m_pass2Edit->text()) {
            m_resultLabel->setText(tr("Passphrases do not match."));
            return;
        }
    }

    QString err;
    if (!ensureKeysDir(&err)) {
        m_resultLabel->setText(err);
        return;
    }

    QString keyName = m_keyNameEdit->text().trimmed();
    if (keyName.isEmpty()) {
        keyName = QString("id_%1_pqssh_%2")
                      .arg(algo, QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss"));
    }

    const QString label   = m_labelEdit->text().trimmed();
    const QString owner   = m_ownerEdit->text().trimmed();
    const QString purpose = m_purposeEdit->text().trimmed();
    const int rotationDays = m_rotationSpin->value();
    const QString status  = m_statusCombo->currentText();

    const QString pass1 = m_pass1Edit->text();
    const QString pass2 = m_pass2Edit->text();
    if (pass1 != pass2) {
        m_resultLabel->setText(tr("Passphrases do not match."));
        return;
    }

    const QString privPath = QDir(keysDir()).filePath(keyName);
    const QString pubPath  = privPath + ".pub";

    QString comment = label;
    if (comment.isEmpty() && !owner.isEmpty())
        comment = owner;

    if (!runSshKeygen(algo, privPath, comment, pass1, &err)) {
        m_resultLabel->setText(err);
        return;
    }

    QString finalPrivPath = privPath;
    if (isDilithium) {
        QFile plainFile(privPath);
        if (!plainFile.open(QIODevice::ReadOnly)) {
            m_resultLabel->setText(tr("Failed to read generated Dilithium private key."));
            return;
        }

        const QByteArray plainKey = plainFile.readAll();
        plainFile.close();

        QByteArray encrypted;
        QString encErr;
        if (!encryptDilithiumKey(plainKey, pass1, &encrypted, &encErr)) {
            m_resultLabel->setText(tr("Encryption failed: %1").arg(encErr));
            return;
        }

        const QString encPath = privPath + ".enc";
        QFile encFile(encPath);
        if (!encFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_resultLabel->setText(tr("Failed to write encrypted Dilithium key."));
            return;
        }
        encFile.write(encrypted);
        encFile.close();

        QFile::remove(privPath);
        finalPrivPath = encPath;
    }

    QString fp;
    if (!computeFingerprint(pubPath, &fp, &err)) {
        m_resultLabel->setText(err);
        return;
    }

    const QString created = isoUtcNow();
    QString expires;
    if (m_expireCheck->isChecked()) {
        expires = m_expireDate->dateTime().toUTC().toString(Qt::ISODateWithMs);
    }

    if (!saveMetadata(fp, label, owner, created, expires, purpose,
                      algo, rotationDays, status, finalPrivPath, pubPath, &err)) {
        m_resultLabel->setText(err);
        return;
    }

    m_resultLabel->setText(
        tr("✅ Key generated\n\n"
           "Fingerprint: %1\n"
           "Private: %2\n"
           "Public:  %3\n"
           "Metadata: %4")
            .arg(fp, finalPrivPath, pubPath, metadataPath())
    );

    refreshKeysTable();
    m_tabs->setCurrentIndex(1);
}

void KeyGeneratorDialog::refreshKeysTable()
{
    m_sortColumn = m_table->horizontalHeader()->sortIndicatorSection();
    m_sortOrder  = m_table->horizontalHeader()->sortIndicatorOrder();

    m_table->setSortingEnabled(false);

    QString autoErr;
    autoExpireMetadataFile(metadataPath(), &autoErr);

    QString err;
    m_inventory = buildInventory(&err);

    m_table->setRowCount(0);

    const QList<KeyRow> rows = m_inventory.values();
    m_table->setRowCount(rows.size());

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    int r = 0;
    for (const KeyRow &k : rows) {
        auto mkItem = [](const QString &s) {
            auto *it = new QTableWidgetItem(s);
            it->setToolTip(s);
            return it;
        };

        const bool isEncFile = k.privPath.endsWith(".enc", Qt::CaseInsensitive);
        const bool isOpenSshEnc = (!isEncFile && !k.privPath.isEmpty())
            ? isOpenSshPrivateKeyEncrypted(k.privPath)
            : false;

        const bool isLocked = isEncFile || isOpenSshEnc;

        QString lockTip;
        if (isEncFile)    lockTip = tr("🔒 Private key is stored encrypted at rest (.enc)");
        if (isOpenSshEnc) lockTip = tr("🔒 OpenSSH private key is passphrase-protected");

        const QString keyFileShown = k.pubPath.isEmpty()
            ? QString()
            : QFileInfo(k.pubPath).fileName();

        bool isExpired = false;
        QDateTime expUtc;
        if (!k.expires.isEmpty()) {
            if (parseIsoUtc(k.expires, expUtc) && expUtc.isValid() && expUtc < nowUtc) {
                isExpired = true;
            }
        }

        QString statusShown = k.status;
        if (isExpired && statusShown != "expired")
            statusShown = "expired";

        QString algoShown = k.algorithm;
        if (isLocked)
            algoShown = QString::fromUtf8("🔒 ") + algoShown;

        m_table->setItem(r, 0, mkItem(k.fingerprint));
        m_table->setItem(r, 1, mkItem(k.label));
        m_table->setItem(r, 2, mkItem(k.owner));
        m_table->setItem(r, 3, mkItem(algoShown));
        m_table->setItem(r, 4, mkItem(statusShown));
        m_table->setItem(r, 5, mkItem(k.created));
        m_table->setItem(r, 6, mkItem(k.expires));
        m_table->setItem(r, 7, mkItem(k.purpose));
        m_table->setItem(r, 8, mkItem(k.rotationDays > 0 ? QString::number(k.rotationDays) : QString()));
        m_table->setItem(r, 9, mkItem(keyFileShown));

        if (isLocked && !lockTip.isEmpty()) {
            if (auto *it = m_table->item(r, 3))
                it->setToolTip(lockTip);
        }

        if (isExpired) {
            const QString tip = tr("Key is expired (expire_date=%1 UTC)")
                                    .arg(expUtc.toString(Qt::ISODateWithMs));
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c)) {
                    it->setForeground(QBrush(QColor("#ef5350")));
                    it->setToolTip(tip);
                }
            }
        }
        else if (!k.hasMetadata) {
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c))
                    it->setForeground(QBrush(QColor("#ffb74d")));
            }
        }
        else if (!k.hasFiles) {
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c))
                    it->setForeground(QBrush(QColor("#ef5350")));
            }
        }

        r++;
    }

    if (!err.isEmpty()) {
        m_keysHintLabel->setText(tr("Inventory warning: %1\nmetadata: %2\nkeys: %3")
                                     .arg(err, metadataPath(), keysDir()));
    } else {
        m_keysHintLabel->setText(tr("metadata: %1\nkeys: %2")
                                     .arg(metadataPath(), keysDir()));
    }

    m_table->setSortingEnabled(true);
    m_table->sortItems(m_sortColumn, m_sortOrder);
    m_table->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);

    onKeySelectionChanged();
}

int KeyGeneratorDialog::selectedTableRow() const
{
    const auto ranges = m_table->selectedRanges();
    if (ranges.isEmpty()) return -1;
    return ranges.first().topRow();
}

KeyGeneratorDialog::KeyRow KeyGeneratorDialog::selectedRow() const
{
    const int row = selectedTableRow();
    if (row < 0) return KeyRow{};

    QTableWidgetItem *fpIt = m_table->item(row, 0);
    if (!fpIt) return KeyRow{};

    const QString fp = fpIt->text().trimmed();
    return m_inventory.value(fp, KeyRow{});
}

void KeyGeneratorDialog::onKeySelectionChanged()
{
    const bool hasSel = selectedTableRow() >= 0;

    KeyRow k = selectedRow();
    const bool hasFp = hasSel && !k.fingerprint.isEmpty();
    const bool hasPub = hasFp && !k.pubPath.isEmpty() && QFile::exists(k.pubPath);

    m_copyFpBtn->setEnabled(hasFp);
    m_copyPubBtn->setEnabled(hasPub);
    m_exportPubBtn->setEnabled(hasPub);

    // ✅ enable only when we truly have a pubkey line to install
    m_installBtn->setEnabled(hasPub && !m_profileNames.isEmpty());

    m_editMetaBtn->setEnabled(hasFp && k.hasMetadata);
    m_revokeBtn->setEnabled(hasFp && k.hasMetadata);
    m_deleteBtn->setEnabled(hasFp);
}

void KeyGeneratorDialog::onCopyFingerprint()
{
    KeyRow k = selectedRow();
    if (k.fingerprint.isEmpty()) return;
    QApplication::clipboard()->setText(k.fingerprint);
}

static bool isOpenSshPrivateKeyEncrypted(const QString &privPath)
{
    QFile f(privPath);
    if (!f.exists()) return false;
    if (!f.open(QIODevice::ReadOnly)) return false;

    const QByteArray text = f.readAll();
    f.close();

    if (text.contains("BEGIN ENCRYPTED PRIVATE KEY")) return true;
    if (text.contains("Proc-Type: 4,ENCRYPTED")) return true;
    if (!text.contains("BEGIN OPENSSH PRIVATE KEY")) return false;

    QByteArray b64;
    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &ln : lines) {
        if (ln.startsWith("-----")) continue;
        const QByteArray t = ln.trimmed();
        if (!t.isEmpty()) b64 += t;
    }

    const QByteArray blob = QByteArray::fromBase64(b64);
    const QByteArray magic("openssh-key-v1\0", 15);
    if (!blob.startsWith(magic)) return false;
    int off = magic.size();

    auto readU32 = [&](quint32 &out) -> bool {
        if (off + 4 > blob.size()) return false;
        out = (quint8(blob[off]) << 24) |
              (quint8(blob[off+1]) << 16) |
              (quint8(blob[off+2]) << 8) |
              (quint8(blob[off+3]));
        off += 4;
        return true;
    };

    auto readString = [&](QByteArray &out) -> bool {
        quint32 len = 0;
        if (!readU32(len)) return false;
        if (off + int(len) > blob.size()) return false;
        out = blob.mid(off, int(len));
        off += int(len);
        return true;
    };

    QByteArray ciphername;
    if (!readString(ciphername)) return false;

    return ciphername != "none";
}

void KeyGeneratorDialog::onCopyPublicKey()
{
    KeyRow k = selectedRow();
    if (k.pubPath.isEmpty() || !QFile::exists(k.pubPath)) return;

    const QString line = readPublicKeyLine(k.pubPath);
    if (line.isEmpty()) return;

    QApplication::clipboard()->setText(line);
}

void KeyGeneratorDialog::onExportPublicKey()
{
    KeyRow k = selectedRow();
    if (k.pubPath.isEmpty() || !QFile::exists(k.pubPath)) return;

    const QString line = readPublicKeyLine(k.pubPath);
    if (line.isEmpty()) return;

    const QString suggested = QFileInfo(k.pubPath).fileName();
    const QString outPath = QFileDialog::getSaveFileName(this, tr("Export public key"), suggested);
    if (outPath.isEmpty()) return;

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export failed"), f.errorString());
        return;
    }
    f.write(line.toUtf8());
    f.write("\n");
    f.close();
}

bool KeyGeneratorDialog::updateMetadataFields(const QString &fingerprint,
                                             const QString &label,
                                             const QString &owner,
                                             const QString &purpose,
                                             int rotationDays,
                                             const QString &expiresIsoOrEmpty,
                                             QString *errOut)
{
    QFile f(metadataPath());
    if (!f.exists()) {
        if (errOut) *errOut = tr("metadata.json does not exist.");
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = tr("Cannot read metadata.json");
        return false;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject()) {
        if (errOut) *errOut = tr("metadata.json is not a JSON object.");
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.value(fingerprint).isObject()) {
        if (errOut) *errOut = tr("Selected fingerprint not found in metadata.json");
        return false;
    }

    QJsonObject meta = root.value(fingerprint).toObject();

    meta["label"] = label;
    meta["owner"] = owner;
    meta["purpose"] = purpose;
    meta["rotation_policy_days"] = rotationDays;

    meta["expire_date"] = expiresIsoOrEmpty.isEmpty()
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(expiresIsoOrEmpty);

    root[fingerprint] = meta;

    return writeMetadata(root, errOut);
}

void KeyGeneratorDialog::onEditMetadata()
{
    KeyRow k = selectedRow();
    if (k.fingerprint.isEmpty() || !k.hasMetadata) return;

    EditMetadataDialog dlg(this);
    dlg.setInitial(k.label, k.owner, k.purpose, k.rotationDays, k.expires);

    if (dlg.exec() != QDialog::Accepted)
        return;

    QString err;
    if (!updateMetadataFields(k.fingerprint,
                             dlg.label(),
                             dlg.owner(),
                             dlg.purpose(),
                             dlg.rotationDays(),
                             dlg.expiresIsoOrEmpty(),
                             &err)) {
        QMessageBox::warning(this, tr("Edit failed"), err);
        return;
    }

    refreshKeysTable();
}

void KeyGeneratorDialog::onMarkRevoked()
{
    KeyRow k = selectedRow();
    if (k.fingerprint.isEmpty()) return;

    QFile f(metadataPath());
    if (!f.exists()) return;

    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Error"), tr("Cannot read metadata.json"));
        return;
    }
    auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;

    QJsonObject root = doc.object();
    if (!root.value(k.fingerprint).isObject()) return;

    QJsonObject meta = root.value(k.fingerprint).toObject();
    meta["status"] = "revoked";
    root[k.fingerprint] = meta;

    QString err;
    if (!writeMetadata(root, &err)) {
        QMessageBox::warning(this, tr("Error"), err);
        return;
    }

    refreshKeysTable();
}

void KeyGeneratorDialog::onDeleteKey()
{
    KeyRow k = selectedRow();
    if (k.fingerprint.isEmpty()) return;

    QString msg = tr("Delete key entry?\n\nFingerprint:\n%1").arg(k.fingerprint);
    if (m_deleteFilesCheck->isChecked())
        msg += "\n\n" + tr("Also delete key files on disk.");

    const auto res = QMessageBox::question(this, tr("Confirm delete"), msg,
                                          QMessageBox::Yes | QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    QFile f(metadataPath());
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, tr("Error"), tr("Cannot read metadata.json"));
            return;
        }
        auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();

        if (doc.isObject()) {
            QJsonObject root = doc.object();
            root.remove(k.fingerprint);
            QString err;
            if (!writeMetadata(root, &err)) {
                QMessageBox::warning(this, tr("Error"), err);
                return;
            }
        }
    }

    if (m_deleteFilesCheck->isChecked()) {
        if (!k.pubPath.isEmpty()) QFile::remove(k.pubPath);
        if (!k.privPath.isEmpty()) QFile::remove(k.privPath);
    }

    refreshKeysTable();
}

void KeyGeneratorDialog::onKeysContextMenuRequested(const QPoint& pos)
{
    if (!m_table) return;

    const QModelIndex idx = m_table->indexAt(pos);
    if (!idx.isValid()) return;

    // Select the row under cursor (so action uses what user clicked)
    m_table->selectRow(idx.row());
    onKeySelectionChanged();

    KeyRow k = selectedRow();
    const bool hasPub = !k.pubPath.isEmpty() && QFile::exists(k.pubPath);

    QMenu menu(this);

    QAction *installAct = menu.addAction(tr("Install selected key…"));
    installAct->setEnabled(hasPub && !m_profileNames.isEmpty());

    if (!hasPub)
        installAct->setToolTip(tr("Selected key has no readable .pub file."));
    else if (m_profileNames.isEmpty())
        installAct->setToolTip(tr("No profiles available."));

    connect(installAct, &QAction::triggered, this, &KeyGeneratorDialog::onInstallSelectedKey);

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

// Install selected key flow:
//
// 1. Validate .pub exists
// 2. Let user choose target profile
// 3. Local confirmation dialog (preview only)
// 4. Emit installPublicKeyRequested(pubLine, profileIndex)
//
// Actual SSH work happens elsewhere.
void KeyGeneratorDialog::onInstallSelectedKey()
{
    KeyRow k = selectedRow();
    if (k.pubPath.isEmpty() || !QFile::exists(k.pubPath)) {
        QMessageBox::warning(this, tr("Install key"), tr("Selected key has no readable public key file."));
        return;
    }

    const QString pubLine = readPublicKeyLine(k.pubPath).trimmed();
    if (pubLine.isEmpty()) {
        QMessageBox::warning(this, tr("Install key"), tr("Public key line is empty."));
        return;
    }

    if (m_profileNames.isEmpty()) {
        QMessageBox::warning(this, tr("Install key"), tr("No profiles available."));
        return;
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this,
        tr("Install selected key"),
        tr("Choose target profile:"),
        m_profileNames,
        0,
        false,
        &ok
    );
    if (!ok || chosen.isEmpty()) return;

    const int profileIndex = m_profileNames.indexOf(chosen);
    if (profileIndex < 0) {
        QMessageBox::warning(this, tr("Install key"), tr("Invalid profile selection."));
        return;
    }

    // Optional: small local sanity preview before handing to MainWindow confirm
    const QStringList parts = pubLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const QString kind = parts.value(0);
    const QString preview = parts.value(1).left(24);

    const QString localConfirm =
        tr("Install this key?\n\n"
           "Key: %1 (%2…)\n"
           "To profile: %3")
            .arg(kind, preview, chosen);

    if (QMessageBox::question(this, tr("Confirm selection"), localConfirm) != QMessageBox::Yes)
        return;

    emit installPublicKeyRequested(pubLine, profileIndex);
}
