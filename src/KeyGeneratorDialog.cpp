#include "KeyGeneratorDialog.h"
#include "KeyMetadataUtils.h"

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
#include <QRandomGenerator>
#include <QBrush>

static QString isoUtcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// ---------------------------------------------------------------------------
// PQSSH helpers (Dilithium5 stub)
// ---------------------------------------------------------------------------

static QString sha256Fingerprint(const QByteArray &data)
{
    const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    QString b64 = QString::fromLatin1(digest.toBase64());
    b64.remove('='); // OpenSSH-style (often no padding)
    return "SHA256:" + b64;
}

static bool runDilithium5StubKeygen(const QString &privPath,
                                   const QString &comment,
                                   const QString &passphrase,
                                   QString *errOut)
{
    Q_UNUSED(passphrase); // (we’ll add encryption later if/when you want)

    // Placeholder sizes (close to ML-DSA-87 / “Dilithium5” typical sizes)
    QByteArray pub(2592, Qt::Uninitialized);
    QByteArray priv(4896, Qt::Uninitialized);

    QRandomGenerator::global()->generate(pub.begin(), pub.end());
    QRandomGenerator::global()->generate(priv.begin(), priv.end());

    // avoid overwrite prompt
    QFile::remove(privPath);
    QFile::remove(privPath + ".pub");

    // Write private key (simple PQSSH format for now)
    {
        QFile f(privPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errOut) *errOut = QString("Failed to write private key: %1").arg(f.errorString());
            return false;
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        f.write("PQSSH-PRIVATE-KEY v1\n");
        f.write("alg:dilithium5\n");
        f.write("encoding:base64\n");
        f.write(priv.toBase64());
        f.write("\n");
        f.close();
    }

    // Write public key line (so your existing copy/export UI works)
    {
        QFile f(privPath + ".pub");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errOut) *errOut = QString("Failed to write public key: %1").arg(f.errorString());
            return false;
        }

        // Format: pqssh-dilithium5 <base64> <comment>
        QByteArray line = "pqssh-dilithium5 ";
        line += pub.toBase64();
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
class EditMetadataDialog : public QDialog
{
public:
    explicit EditMetadataDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle("Edit Key Metadata");
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

        expireCheck = new QCheckBox("Enable", this);
        expireDate = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addDays(90), this);
        expireDate->setDisplayFormat("yyyy-MM-dd HH:mm 'UTC'");
        expireDate->setTimeSpec(Qt::UTC);
        expireDate->setEnabled(false);

        QObject::connect(expireCheck, &QCheckBox::toggled, expireDate, &QWidget::setEnabled);

        expireLay->addWidget(expireCheck);
        expireLay->addWidget(expireDate, 1);

        form->addRow("Label:", labelEdit);
        form->addRow("Owner:", ownerEdit);
        form->addRow("Purpose:", purposeEdit);
        form->addRow("Rotation policy (days):", rotationSpin);
        form->addRow("Expire date:", expireRow);

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

static bool isExpiredNow(const QString &expiresIso)
{
    QDateTime expUtc;
    if (!parseIsoUtc(expiresIso, expUtc)) return false;
    return expUtc < QDateTime::currentDateTimeUtc();
}

// ============================================================================
// KeyGeneratorDialog
// ============================================================================

KeyGeneratorDialog::KeyGeneratorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Key Generator");
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
    // Added dilithium5
    m_algoCombo->addItems({ "ed25519", "rsa3072", "rsa4096", "dilithium5" });
    form->addRow("Algorithm:", m_algoCombo);

    m_keyNameEdit = new QLineEdit(this);
    m_keyNameEdit->setPlaceholderText("id_ed25519_pqssh");
    form->addRow("Key filename:", m_keyNameEdit);

    m_labelEdit = new QLineEdit(this);
    form->addRow("Label:", m_labelEdit);

    m_ownerEdit = new QLineEdit(this);
    const QString envUser = qEnvironmentVariable("USER");
    if (!envUser.isEmpty()) m_ownerEdit->setText(envUser);
    form->addRow("Owner:", m_ownerEdit);

    m_purposeEdit = new QLineEdit(this);
    m_purposeEdit->setPlaceholderText("e.g. admin, prod-access, ci-deployment");
    form->addRow("Purpose:", m_purposeEdit);

    m_rotationSpin = new QSpinBox(this);
    m_rotationSpin->setRange(0, 3650);
    m_rotationSpin->setValue(90);
    form->addRow("Rotation policy (days):", m_rotationSpin);

    m_statusCombo = new QComboBox(this);
    m_statusCombo->addItems({ "active", "revoked", "expired" });
    m_statusCombo->setCurrentText("active");
    form->addRow("Status:", m_statusCombo);

    // Expiration row
    auto *expireRow = new QWidget(this);
    auto *expireLayout = new QHBoxLayout(expireRow);
    expireLayout->setContentsMargins(0,0,0,0);

    m_expireCheck = new QCheckBox("Enable", this);
    m_expireDate = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addDays(90), this);
    m_expireDate->setDisplayFormat("yyyy-MM-dd HH:mm 'UTC'");
    m_expireDate->setTimeSpec(Qt::UTC);
    m_expireDate->setEnabled(false);

    connect(m_expireCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_expireDate->setEnabled(on);
    });

    expireLayout->addWidget(m_expireCheck);
    expireLayout->addWidget(m_expireDate, 1);
    form->addRow("Expire date:", expireRow);

    // Passphrase
    m_pass1Edit = new QLineEdit(this);
    m_pass1Edit->setEchoMode(QLineEdit::Password);
    m_pass2Edit = new QLineEdit(this);
    m_pass2Edit->setEchoMode(QLineEdit::Password);
    form->addRow("Passphrase:", m_pass1Edit);
    form->addRow("Passphrase (again):", m_pass2Edit);

    genLayout->addLayout(form);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setText(QString("Keys are saved under:\n%1").arg(keysDir()));
    genLayout->addWidget(m_resultLabel);

    auto *genButtonsRow = new QHBoxLayout();
    genButtonsRow->addStretch(1);
    m_generateBtn = new QPushButton("Generate", this);
    genButtonsRow->addWidget(m_generateBtn);
    genLayout->addLayout(genButtonsRow);

    connect(m_generateBtn, &QPushButton::clicked, this, &KeyGeneratorDialog::onGenerate);

    m_tabs->addTab(genTab, "Generate");

    // ============================
    // TAB 2: Keys (Manage)
    // ============================
    auto *keysTab = new QWidget(this);
    auto *keysLayout = new QVBoxLayout(keysTab);

    m_keysHintLabel = new QLabel(this);
    m_keysHintLabel->setWordWrap(true);
    m_keysHintLabel->setText(QString("Inventory:\n- metadata: %1\n- key files: %2/*.pub")
                                 .arg(metadataPath(), keysDir()));
    keysLayout->addWidget(m_keysHintLabel);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(10);
    m_table->setHorizontalHeaderLabels({
        "Fingerprint", "Label", "Owner", "Algorithm", "Status",
        "Created", "Expires", "Purpose", "Rotation", "Key file"
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);

    keysLayout->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    m_refreshBtn   = new QPushButton("Refresh", this);
    m_copyFpBtn    = new QPushButton("Copy fingerprint", this);
    m_copyPubBtn   = new QPushButton("Copy public key", this);
    m_exportPubBtn = new QPushButton("Export pubkey...", this);
    m_editMetaBtn  = new QPushButton("Edit metadata...", this);
    m_revokeBtn    = new QPushButton("Mark revoked", this);
    m_deleteBtn    = new QPushButton("Delete", this);
    m_deleteFilesCheck = new QCheckBox("Also delete key files", this);

    btnRow->addWidget(m_refreshBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_copyFpBtn);
    btnRow->addWidget(m_copyPubBtn);
    btnRow->addWidget(m_exportPubBtn);
    btnRow->addWidget(m_editMetaBtn);
    btnRow->addWidget(m_revokeBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addWidget(m_deleteFilesCheck);

    keysLayout->addLayout(btnRow);

    connect(m_refreshBtn,   &QPushButton::clicked, this, &KeyGeneratorDialog::refreshKeysTable);
    connect(m_copyFpBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onCopyFingerprint);
    connect(m_copyPubBtn,   &QPushButton::clicked, this, &KeyGeneratorDialog::onCopyPublicKey);
    connect(m_exportPubBtn, &QPushButton::clicked, this, &KeyGeneratorDialog::onExportPublicKey);
    connect(m_editMetaBtn,  &QPushButton::clicked, this, &KeyGeneratorDialog::onEditMetadata);
    connect(m_revokeBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onMarkRevoked);
    connect(m_deleteBtn,    &QPushButton::clicked, this, &KeyGeneratorDialog::onDeleteKey);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, &KeyGeneratorDialog::onKeySelectionChanged);

    m_tabs->addTab(keysTab, "Keys");

    // Bottom close
    auto *buttons = new QDialogButtonBox(this);
    auto *closeBtn = buttons->addButton("Close", QDialogButtonBox::RejectRole);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    root->addWidget(buttons);

    // Initial fill
    refreshKeysTable();
    onKeySelectionChanged();
}

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
        if (errOut) *errOut = QString("Failed to create keys directory: %1").arg(keysDir());
        return false;
    }
    return true;
}

bool KeyGeneratorDialog::runSshKeygen(const QString &algo, const QString &privPath,
                                     const QString &comment, const QString &passphrase,
                                     QString *errOut)
{
    // Route PQ keygen here (does not use ssh-keygen)
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
        if (errOut) *errOut = QString("Unsupported algorithm: %1").arg(algo);
        return false;
    }

    // avoid overwrite prompt
    QFile::remove(privPath);
    QFile::remove(privPath + ".pub");

    args << "-f" << privPath;
    if (!comment.isEmpty()) args << "-C" << comment;
    args << "-N" << passphrase;

    QProcess p;
    p.start("ssh-keygen", args);
    if (!p.waitForFinished(30000)) {
        if (errOut) *errOut = "ssh-keygen timed out.";
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (errOut) *errOut = QString("ssh-keygen failed:\n%1")
            .arg(QString::fromLocal8Bit(p.readAllStandardError()));
        return false;
    }
    return true;
}

bool KeyGeneratorDialog::computeFingerprint(const QString &pubPath, QString *fpOut, QString *errOut)
{
    // 1) Try OpenSSH fingerprinting first (ed25519/rsa keys)
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
        // fallthrough to PQSSH parsing
    }

    // 2) Fallback: PQSSH pubkey format: pqssh-dilithium5 <base64> <comment...>
    QFile f(pubPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "Cannot read public key: " + f.errorString();
        return false;
    }
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    f.close();

    const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        if (errOut) *errOut = QString("Unrecognized public key format:\n%1").arg(line);
        return false;
    }

    const QString kind = parts[0];
    const QString b64  = parts[1];

    if (!kind.startsWith("pqssh-")) {
        if (errOut) *errOut = QString("Unsupported public key format (not OpenSSH, not PQSSH): %1").arg(kind);
        return false;
    }

    const QByteArray pub = QByteArray::fromBase64(b64.toLatin1());
    if (pub.isEmpty()) {
        if (errOut) *errOut = "Invalid base64 public key data.";
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
        if (errOut) *errOut = "Cannot read metadata.json";
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
        if (errOut) *errOut = "Cannot write metadata.json";
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
            if (errOut) *errOut = "Cannot read metadata.json";
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

    // Extra fields help management/export/delete
    meta["private_key_path"] = privPath;
    meta["public_key_path"]  = pubPath;

    // Index by fingerprint
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

QMap<QString, KeyGeneratorDialog::KeyRow> KeyGeneratorDialog::buildInventory(QString *errOut)
{
    QMap<QString, KeyRow> inv;

    // 1) scan *.pub files
    QDir d(keysDir());
    const QStringList pubs = d.entryList({ "*.pub" }, QDir::Files, QDir::Name);
    for (const QString &pubFile : pubs) {
        const QString pubPath = d.filePath(pubFile);

        const QString pubLine = readPublicKeyLine(pubPath);

        // Try to infer algorithm from pubkey line (helps when metadata.json missing)
        QString inferredAlgo;
        if (!pubLine.isEmpty()) {
            const QStringList parts = pubLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const QString kind = parts.isEmpty() ? QString() : parts[0];

            if (kind == "pqssh-dilithium5") {
                inferredAlgo = "dilithium5";
            } else if (kind == "ssh-ed25519") {
                inferredAlgo = "ed25519";
            } else if (kind == "ssh-rsa") {
                // Could be rsa3072 or rsa4096; unknown without metadata, keep generic
                inferredAlgo = "rsa";
            } else if (kind.startsWith("pqssh-")) {
                inferredAlgo = kind.mid(QString("pqssh-").size());
            }
        }

        // Infer created time from pub file mtime (useful when metadata missing)
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
            continue; // ignore one broken pub
        }

        KeyRow row;
        row.fingerprint = fp;
        row.pubPath = pubPath;
        row.privPath = pubPath.left(pubPath.size() - 4); // remove ".pub"
        row.comment = pubLine;
        row.algorithm = inferredAlgo;
        row.created = inferredCreated;   // <-- added
        row.hasFiles = true;

        inv[fp] = row;
    }


    // 2) merge metadata
    QMap<QString, QJsonObject> metaMap;
    QString metaErr;
    if (!loadMetadata(&metaMap, &metaErr)) {
        if (errOut) *errOut = metaErr;
        // still show file inventory
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
    QString err;
    if (!ensureKeysDir(&err)) {
        m_resultLabel->setText(err);
        return;
    }

    const QString algo = m_algoCombo->currentText();

    QString keyName = m_keyNameEdit->text().trimmed();
    if (keyName.isEmpty()) {
        keyName = QString("id_%1_pqssh_%2")
            .arg(algo, QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss"));
    }

    const QString label = m_labelEdit->text().trimmed();
    const QString owner = m_ownerEdit->text().trimmed();
    const QString purpose = m_purposeEdit->text().trimmed();
    const int rotationDays = m_rotationSpin->value();
    const QString status = m_statusCombo->currentText();

    const QString pass1 = m_pass1Edit->text();
    const QString pass2 = m_pass2Edit->text();
    if (pass1 != pass2) {
        m_resultLabel->setText("Passphrases do not match.");
        return;
    }

    const QString privPath = QDir(keysDir()).filePath(keyName);
    const QString pubPath  = privPath + ".pub";

    QString comment = label;
    if (comment.isEmpty() && !owner.isEmpty()) comment = owner;

    if (!runSshKeygen(algo, privPath, comment, pass1, &err)) {
        m_resultLabel->setText(err);
        return;
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

    if (!saveMetadata(fp, label, owner, created, expires, purpose, algo, rotationDays, status, privPath, pubPath, &err)) {
        m_resultLabel->setText(err);
        return;
    }

    m_resultLabel->setText(
        QString("✅ Key generated\n\n"
                "Fingerprint: %1\n"
                "Private: %2\n"
                "Public:  %3\n"
                "Metadata: %4")
            .arg(fp, privPath, pubPath, metadataPath())
    );

    refreshKeysTable();
    m_tabs->setCurrentIndex(1);
}

void KeyGeneratorDialog::refreshKeysTable()
{
    QString autoErr;
    autoExpireMetadataFile(metadataPath(), &autoErr);
    // keep autoErr silent unless you want to show it

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

        const QString keyFileShown = k.pubPath.isEmpty()
            ? QString()
            : QFileInfo(k.pubPath).fileName();

        // --- Compute expired? (effective state) ---
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

        m_table->setItem(r, 0, mkItem(k.fingerprint));
        m_table->setItem(r, 1, mkItem(k.label));
        m_table->setItem(r, 2, mkItem(k.owner));
        m_table->setItem(r, 3, mkItem(k.algorithm));
        m_table->setItem(r, 4, mkItem(statusShown));
        m_table->setItem(r, 5, mkItem(k.created));
        m_table->setItem(r, 6, mkItem(k.expires));
        m_table->setItem(r, 7, mkItem(k.purpose));
        m_table->setItem(r, 8, mkItem(k.rotationDays > 0 ? QString::number(k.rotationDays) : QString()));
        m_table->setItem(r, 9, mkItem(keyFileShown));

        // --- Row coloring priority ---
        if (isExpired) {
            const QString tip = QString("Key is expired (expire_date=%1 UTC)").arg(expUtc.toString(Qt::ISODateWithMs));
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c)) {
                    it->setForeground(QBrush(QColor("#ef5350"))); // red
                    it->setToolTip(tip);
                }
            }
        }
        else if (!k.hasMetadata) {
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c))
                    it->setForeground(QBrush(QColor("#ffb74d"))); // orange
            }
        }
        else if (!k.hasFiles) {
            for (int c = 0; c < m_table->columnCount(); ++c) {
                if (auto *it = m_table->item(r, c))
                    it->setForeground(QBrush(QColor("#ef5350"))); // red
            }
        }

        r++;
    }

    if (!err.isEmpty()) {
        m_keysHintLabel->setText(QString("Inventory warning: %1\nmetadata: %2\nkeys: %3")
                                     .arg(err, metadataPath(), keysDir()));
    } else {
        m_keysHintLabel->setText(QString("metadata: %1\nkeys: %2")
                                     .arg(metadataPath(), keysDir()));
    }

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
    const QString outPath = QFileDialog::getSaveFileName(this, "Export public key", suggested);
    if (outPath.isEmpty()) return;

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Export failed", f.errorString());
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
        if (errOut) *errOut = "metadata.json does not exist.";
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = "Cannot read metadata.json";
        return false;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject()) {
        if (errOut) *errOut = "metadata.json is not a JSON object.";
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.value(fingerprint).isObject()) {
        if (errOut) *errOut = "Selected fingerprint not found in metadata.json";
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
        QMessageBox::warning(this, "Edit failed", err);
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
        QMessageBox::warning(this, "Error", "Cannot read metadata.json");
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
        QMessageBox::warning(this, "Error", err);
        return;
    }

    refreshKeysTable();
}

void KeyGeneratorDialog::onDeleteKey()
{
    KeyRow k = selectedRow();
    if (k.fingerprint.isEmpty()) return;

    QString msg = QString("Delete key entry?\n\nFingerprint:\n%1").arg(k.fingerprint);
    if (m_deleteFilesCheck->isChecked())
        msg += "\n\nAlso delete key files on disk.";

    const auto res = QMessageBox::question(this, "Confirm delete", msg,
                                          QMessageBox::Yes | QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    // Remove metadata entry
    QFile f(metadataPath());
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Error", "Cannot read metadata.json");
            return;
        }
        auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();

        if (doc.isObject()) {
            QJsonObject root = doc.object();
            root.remove(k.fingerprint);
            QString err;
            if (!writeMetadata(root, &err)) {
                QMessageBox::warning(this, "Error", err);
                return;
            }
        }
    }

    // Optionally delete key files
    if (m_deleteFilesCheck->isChecked()) {
        if (!k.pubPath.isEmpty()) QFile::remove(k.pubPath);
        if (!k.privPath.isEmpty()) QFile::remove(k.privPath);
    }

    refreshKeysTable();
}
