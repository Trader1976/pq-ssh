#include "ProfilesEditorDialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QAbstractItemView>
#include <QStringList>
#include <QSet>
#include <QWidget>
#include <QFileDialog>
#include <QToolButton>
#include <QDir>

#include "CpunkTermWidget.h"

ProfilesEditorDialog::ProfilesEditorDialog(const QVector<SshProfile> &profiles, QWidget *parent)
    : QDialog(parent),
      m_working(profiles)
{
    setWindowTitle("Manage SSH Profiles");
    resize(750, 500);

    buildUi();

    if (!m_working.isEmpty()) {
        m_list->setCurrentRow(0);
        onListRowChanged(0);
    } else {
        loadProfileToForm(-1);
    }
}

static QStringList allTermSchemes()
{
    // Create a probe widget just to query available schemes
    CpunkTermWidget probe(0, nullptr);
    QStringList schemes = probe.availableColorSchemes();

    schemes.removeDuplicates();
    schemes.sort(Qt::CaseInsensitive);

    return schemes;
}

static void fillSchemeCombo(QComboBox *combo)
{
    if (!combo) return;

    const QString current = combo->currentText();

    // Your pinned favorites first (only if installed)
    const QStringList pinned = {
        "WhiteOnBlack",
        "Ubuntu",
        "BreezeModified",
        "DarkPastels",
        "Tango",
        "Linux",
        "Solarized",
        "SolarizedLight",
        "BlackOnLightYellow",
        "BlackOnWhite",
        "GreenOnBlack"
    };

    QStringList schemes = allTermSchemes();

    combo->clear();

    QSet<QString> used;

    // Add pinned (if present)
    for (const QString &s : pinned) {
        if (schemes.contains(s) && !used.contains(s)) {
            combo->addItem(s);
            used.insert(s);
        }
    }

    // Separator if we added any pinned items
    if (!used.isEmpty() && schemes.size() > used.size())
        combo->insertSeparator(combo->count());

    // Add the rest
    for (const QString &s : schemes) {
        if (!used.contains(s)) {
            combo->addItem(s);
            used.insert(s);
        }
    }

    // Restore selection if possible
    if (!current.isEmpty()) {
        const int idx = combo->findText(current);
        if (idx >= 0) combo->setCurrentIndex(idx);
    }
}

static QStringList installedSchemes()
{
    CpunkTermWidget probe(0, nullptr);
    QStringList schemes = probe.availableColorSchemes();
    schemes.removeDuplicates();
    schemes.sort(Qt::CaseInsensitive);
    return schemes;
}

static void populateSchemeCombo(QComboBox *combo)
{
    if (!combo) return;

    // Keep some favorites at the top if they exist
    const QStringList pinned = {
        "CPUNK-DNA",
        "CPUNK-Aurora",
        "WhiteOnBlack",
        "Ubuntu",
        "DarkPastels",
        "Tango",
        "Linux",
        "Solarized",
        "SolarizedLight",
        "BlackOnLightYellow",
        "BlackOnWhite",
        "GreenOnBlack"
    };

    const QString current = combo->currentText();
    QStringList schemes = installedSchemes();

    combo->clear();

    QSet<QString> used;

    for (const QString &s : pinned) {
        if (schemes.contains(s) && !used.contains(s)) {
            combo->addItem(s);
            used.insert(s);
        }
    }

    if (!used.isEmpty() && schemes.size() > used.size())
        combo->insertSeparator(combo->count());

    for (const QString &s : schemes) {
        if (!used.contains(s))
            combo->addItem(s);
    }

    if (!current.isEmpty()) {
        int idx = combo->findText(current);
        if (idx >= 0) combo->setCurrentIndex(idx);
    }
}

void ProfilesEditorDialog::buildUi()
{
    auto *mainLayout = new QHBoxLayout(this);

    // ---------- Left: profile list + Add/Delete ----------
    auto *leftWidget = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);

    auto *listLabel = new QLabel("Profiles", leftWidget);
    listLabel->setStyleSheet("font-weight: bold;");

    m_list = new QListWidget(leftWidget);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    for (const auto &p : m_working)
        m_list->addItem(p.name);

    auto *buttonsRow = new QWidget(leftWidget);
    auto *buttonsLayout = new QHBoxLayout(buttonsRow);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(6);

    auto *addBtn = new QPushButton("Add", buttonsRow);
    auto *delBtn = new QPushButton("Delete", buttonsRow);

    buttonsLayout->addWidget(addBtn);
    buttonsLayout->addWidget(delBtn);

    leftLayout->addWidget(listLabel);
    leftLayout->addWidget(m_list, 1);
    leftLayout->addWidget(buttonsRow, 0);

    // ---------- Right: profile details form ----------
    auto *rightWidget = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    m_nameEdit = new QLineEdit(rightWidget);
    m_userEdit = new QLineEdit(rightWidget);
    m_hostEdit = new QLineEdit(rightWidget);

    m_portSpin = new QSpinBox(rightWidget);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);

    m_pqDebugCheck = new QCheckBox("Enable PQ debug (-vv)", rightWidget);

    m_colorSchemeCombo = new QComboBox(rightWidget);
    fillSchemeCombo(m_colorSchemeCombo);

    m_fontSizeSpin = new QSpinBox(rightWidget);
    m_fontSizeSpin->setRange(6, 32);
    m_fontSizeSpin->setValue(11);

    m_widthSpin = new QSpinBox(rightWidget);
    m_widthSpin->setRange(400, 4000);
    m_widthSpin->setValue(900);

    m_heightSpin = new QSpinBox(rightWidget);
    m_heightSpin->setRange(300, 3000);
    m_heightSpin->setValue(500);

    // --- NEW: Key file + Key type ---
    m_keyTypeCombo = new QComboBox(rightWidget);
    m_keyTypeCombo->addItem("auto");
    m_keyTypeCombo->addItem("openssh");
    m_keyTypeCombo->addItem("pq"); // placeholder for later; you can add dilithium5/mldsa87/etc later

    m_keyFileEdit = new QLineEdit(rightWidget);
    m_keyFileEdit->setPlaceholderText("e.g. /home/timo/.ssh/id_ed25519 (optional)");

    auto *browseBtn = new QToolButton(rightWidget);
    browseBtn->setText("...");

    auto *keyRow = new QWidget(rightWidget);
    auto *keyRowLayout = new QHBoxLayout(keyRow);
    keyRowLayout->setContentsMargins(0, 0, 0, 0);
    keyRowLayout->setSpacing(6);
    keyRowLayout->addWidget(m_keyFileEdit, 1);
    keyRowLayout->addWidget(browseBtn, 0);

    connect(browseBtn, &QToolButton::clicked, this, [this]() {
        const QString startDir = QDir::homePath() + "/.ssh";
        const QString path = QFileDialog::getOpenFileName(
            this,
            "Select private key file",
            startDir,
            "Key files (*)"
        );
        if (!path.isEmpty())
            m_keyFileEdit->setText(path);
    });

    form->addRow("Name:", m_nameEdit);
    form->addRow("User:", m_userEdit);
    form->addRow("Host:", m_hostEdit);
    form->addRow("Port:", m_portSpin);
    form->addRow("", m_pqDebugCheck);
    form->addRow("Color scheme:", m_colorSchemeCombo);
    form->addRow("Font size:", m_fontSizeSpin);
    form->addRow("Window width:", m_widthSpin);
    form->addRow("Window height:", m_heightSpin);

    // Add auth fields near host/user/port (feel free to move them higher if you want)
    form->addRow("Key type:", m_keyTypeCombo);
    form->addRow("Key file:", keyRow);

    rightLayout->addLayout(form);

    m_buttonsBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, rightWidget);
    rightLayout->addWidget(m_buttonsBox);

    // Put both sides into main layout
    mainLayout->addWidget(leftWidget, 1);
    mainLayout->addWidget(rightWidget, 2);

    // Wiring
    connect(m_list, &QListWidget::currentRowChanged,
            this, &ProfilesEditorDialog::onListRowChanged);

    connect(m_nameEdit, &QLineEdit::textChanged,
            this, &ProfilesEditorDialog::onNameEdited);

    connect(addBtn, &QPushButton::clicked,
            this, &ProfilesEditorDialog::addProfile);

    connect(delBtn, &QPushButton::clicked,
            this, &ProfilesEditorDialog::deleteProfile);

    connect(m_buttonsBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    connect(m_buttonsBox, &QDialogButtonBox::accepted,
            this, &ProfilesEditorDialog::onAccepted);
}

void ProfilesEditorDialog::onListRowChanged(int row)
{
    syncFormToCurrent();
    m_currentRow = row;
    loadProfileToForm(row);
}

void ProfilesEditorDialog::loadProfileToForm(int row)
{
    if (row < 0 || row >= m_working.size()) {
        m_nameEdit->clear();
        m_userEdit->clear();
        m_hostEdit->clear();
        m_portSpin->setValue(22);
        m_pqDebugCheck->setChecked(true);
        m_colorSchemeCombo->setCurrentText("WhiteOnBlack");
        m_fontSizeSpin->setValue(11);
        m_widthSpin->setValue(900);
        m_heightSpin->setValue(500);

        // NEW: key auth
        if (m_keyTypeCombo) m_keyTypeCombo->setCurrentText("auto");
        if (m_keyFileEdit) m_keyFileEdit->clear();
        return;
    }

    const SshProfile &p = m_working[row];

    m_nameEdit->setText(p.name);
    m_userEdit->setText(p.user);
    m_hostEdit->setText(p.host);
    m_portSpin->setValue(p.port);
    m_pqDebugCheck->setChecked(p.pqDebug);

    const QString wanted = p.termColorScheme.isEmpty()
                               ? QStringLiteral("WhiteOnBlack")
                               : p.termColorScheme;

    int idx = m_colorSchemeCombo->findText(wanted);
    if (idx >= 0)
        m_colorSchemeCombo->setCurrentIndex(idx);
    else
        m_colorSchemeCombo->setCurrentText(wanted);

    m_fontSizeSpin->setValue(p.termFontSize > 0 ? p.termFontSize : 11);
    m_widthSpin->setValue(p.termWidth > 0 ? p.termWidth : 900);
    m_heightSpin->setValue(p.termHeight > 0 ? p.termHeight : 500);

    // NEW: key auth
    if (m_keyTypeCombo) {
        const QString kt = p.keyType.trimmed().isEmpty() ? QString("auto") : p.keyType.trimmed();
        const int kidx = m_keyTypeCombo->findText(kt);
        if (kidx >= 0) m_keyTypeCombo->setCurrentIndex(kidx);
        else m_keyTypeCombo->setCurrentText(kt);
    }
    if (m_keyFileEdit) {
        m_keyFileEdit->setText(p.keyFile);
    }
}

void ProfilesEditorDialog::syncFormToCurrent()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    SshProfile &p = m_working[m_currentRow];

    p.name    = m_nameEdit->text().trimmed();
    p.user    = m_userEdit->text().trimmed();
    p.host    = m_hostEdit->text().trimmed();
    p.port    = m_portSpin->value();
    p.pqDebug = m_pqDebugCheck->isChecked();

    p.termColorScheme = m_colorSchemeCombo->currentText();
    p.termFontSize    = m_fontSizeSpin->value();

    p.termWidth  = m_widthSpin->value();
    p.termHeight = m_heightSpin->value();

    // NEW: key auth
    if (m_keyTypeCombo) {
        const QString kt = m_keyTypeCombo->currentText().trimmed();
        p.keyType = kt.isEmpty() ? QString("auto") : kt;
    }
    if (m_keyFileEdit) {
        p.keyFile = m_keyFileEdit->text().trimmed();
    }

    if (p.name.isEmpty())
        p.name = QString("%1@%2").arg(p.user, p.host);

    if (QListWidgetItem *it = m_list->item(m_currentRow))
        it->setText(p.name);
}

void ProfilesEditorDialog::onNameEdited(const QString &text)
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    m_working[m_currentRow].name = text;

    if (QListWidgetItem *it = m_list->item(m_currentRow)) {
        if (text.trimmed().isEmpty()) {
            it->setText(QString("%1@%2").arg(m_userEdit->text(), m_hostEdit->text()));
        } else {
            it->setText(text);
        }
    }
}

void ProfilesEditorDialog::addProfile()
{
    SshProfile p;
    p.user = qEnvironmentVariable("USER", "user");
    p.host = "localhost";
    p.port = 22;
    p.pqDebug = true;

    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize = 11;
    p.termWidth = 900;
    p.termHeight = 500;

    // NEW: key auth defaults
    p.keyFile = "";
    p.keyType = "auto";

    p.name = QString("%1@%2").arg(p.user, p.host);

    m_working.push_back(p);
    m_list->addItem(p.name);

    int row = m_working.size() - 1;
    m_list->setCurrentRow(row);
}

void ProfilesEditorDialog::deleteProfile()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_working.size())
        return;

    m_working.remove(row);
    delete m_list->takeItem(row);

    if (m_working.isEmpty()) {
        m_currentRow = -1;
        loadProfileToForm(-1);
        return;
    }

    const int newRow = qMin(row, m_working.size() - 1);
    m_list->setCurrentRow(newRow);
}

bool ProfilesEditorDialog::validateProfiles(QString *errMsg) const
{
    for (const auto &p : m_working) {
        if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
            if (errMsg) *errMsg = "Each profile must have non-empty user and host.";
            return false;
        }

        // Optional sanity: if key type != auto and key file is empty -> warn
        const QString kt = p.keyType.trimmed();
        if (!kt.isEmpty() && kt != "auto") {
            if (p.keyFile.trimmed().isEmpty()) {
                if (errMsg) *errMsg = "Key type is set but key file is empty. Either set a key file or set key type to auto.";
                return false;
            }
        }
    }
    if (errMsg) errMsg->clear();
    return true;
}

void ProfilesEditorDialog::onAccepted()
{
    syncFormToCurrent();

    QString err;
    if (!validateProfiles(&err)) {
        QMessageBox::warning(this, "Invalid profile", err);
        return;
    }

    m_result = m_working;
    accept();
}
