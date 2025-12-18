// ProfilesEditorDialog.cpp
//
// UI dialog for managing SSH profiles (create/edit/delete).
// This is a *working copy editor*: changes are made to m_working and only committed
// to m_result when the user presses Save (accepted).
//
// Design goals:
// - Keep profile data model (SshProfile) separate from MainWindow.
// - No direct disk I/O here; saving happens after validation when accepted.
// - Provide terminal scheme selection from qtermwidget’s installed schemes.
// - Persist key auth fields (key_type + key_file) but gate unsupported types elsewhere.
//
// Added (Dec 2025):
// - Group field (empty => "Ungrouped").
// - Group selector is an editable dropdown populated from existing groups.
// - Dialog made wider.

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

// -----------------------------
// Terminal scheme discovery helpers
// -----------------------------
static QStringList allTermSchemes()
{
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

    // Favourites pinned at the top (only if installed)
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

    // Add separator if there are “other” schemes
    if (!used.isEmpty() && schemes.size() > used.size())
        combo->insertSeparator(combo->count());

    // Add the rest
    for (const QString &s : schemes) {
        if (!used.contains(s)) {
            combo->addItem(s);
            used.insert(s);
        }
    }

    // Restore previous selection if possible
    if (!current.isEmpty()) {
        const int idx = combo->findText(current);
        if (idx >= 0) combo->setCurrentIndex(idx);
    }
}

// These two helpers are essentially duplicates of the ones above. Keeping them is fine,
// but long-term you can consolidate to one set to avoid drift.
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

    // Pinned favourites first (only if installed)
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

// -----------------------------
// Group helpers
// -----------------------------
static QString normalizedGroup(const QString &g)
{
    const QString s = g.trimmed();
    return s.isEmpty() ? QStringLiteral("Ungrouped") : s;
}

static QStringList collectGroups(const QVector<SshProfile> &profiles)
{
    QSet<QString> set;
    for (const auto &p : profiles) {
        set.insert(normalizedGroup(p.group));
    }
    QStringList out = QStringList(set.begin(), set.end());
    out.sort(Qt::CaseInsensitive);
    return out;
}

static void populateGroupCombo(QComboBox *combo,
                               const QVector<SshProfile> &profiles,
                               const QString &currentText = QString())
{
    if (!combo) return;

    const QString keep = currentText.isEmpty() ? combo->currentText() : currentText;

    combo->blockSignals(true);
    combo->clear();

    // Always include Ungrouped first
    combo->addItem(QStringLiteral("Ungrouped"));

    const QStringList groups = collectGroups(profiles);
    for (const QString &g : groups) {
        if (g.compare("Ungrouped", Qt::CaseInsensitive) == 0) continue;
        combo->addItem(g);
    }

    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // Restore selection / text
    const QString want = normalizedGroup(keep);
    const int idx = combo->findText(want, Qt::MatchFixedString);
    if (idx >= 0) combo->setCurrentIndex(idx);
    else combo->setCurrentText(want);

    combo->blockSignals(false);
}

// -----------------------------
// Constructor
// -----------------------------
//
// profiles: full list from caller (MainWindow / ProfileStore)
// initialRow: which row should be selected when opening
ProfilesEditorDialog::ProfilesEditorDialog(const QVector<SshProfile> &profiles,
                                          int initialRow,
                                          QWidget *parent)
    : QDialog(parent),
      m_working(profiles) // local working copy: edits do not touch caller until accepted
{
    setWindowTitle("Manage SSH Profiles");
    resize(980, 560);
    setMinimumWidth(900);

    buildUi();

    // Select the same profile the user had selected in MainWindow.
    int row = initialRow;

    if (row < 0 || row >= m_working.size()) {
        row = m_working.isEmpty() ? -1 : 0;
    }

    if (row >= 0) {
        m_list->setCurrentRow(row);
        onListRowChanged(row);
    } else {
        // No profiles at all -> show empty form
        loadProfileToForm(-1);
    }
}

// -----------------------------
// UI Construction
// -----------------------------
//
// Layout is a simple 2-column split:
// - Left: list of profiles + Add/Delete
// - Right: form editing fields for selected profile
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

    // Populate list entries from working copy
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

    // Basic connection fields
    m_nameEdit = new QLineEdit(rightWidget);
    m_userEdit = new QLineEdit(rightWidget);
    m_hostEdit = new QLineEdit(rightWidget);

    m_portSpin = new QSpinBox(rightWidget);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);

    // NEW: group selector (editable dropdown)
    m_groupCombo = new QComboBox(rightWidget);
    m_groupCombo->setEditable(true);
    m_groupCombo->setInsertPolicy(QComboBox::NoInsert);
    m_groupCombo->setToolTip("Group name for sorting in main window (empty = Ungrouped)");
    m_groupCombo->lineEdit()->setPlaceholderText("Ungrouped");

    populateGroupCombo(m_groupCombo, m_working);

    // This currently toggles extra verbosity in the app; it should not expose secrets.
    m_pqDebugCheck = new QCheckBox("Enable PQ debug (-vv)", rightWidget);

    // Terminal appearance
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

    // Scrollback lines: 0 = unlimited, otherwise bounded buffer
    m_historySpin = new QSpinBox(rightWidget);
    m_historySpin->setRange(0, 50000);
    m_historySpin->setSingleStep(500);
    m_historySpin->setValue(2000);
    m_historySpin->setToolTip("Terminal scrollback buffer lines (0 = unlimited)");

    // --- Key-based auth fields ---
    // Note: we store key_type "pq" as a placeholder; SshClient currently only accepts
    // "auto" or "openssh" (it rejects others). This dialog just edits data.
    m_keyTypeCombo = new QComboBox(rightWidget);
    m_keyTypeCombo->addItem("auto");
    m_keyTypeCombo->addItem("openssh");
    m_keyTypeCombo->addItem("pq"); // placeholder for later (dilithium5/mldsa87/etc)

    m_keyFileEdit = new QLineEdit(rightWidget);
    m_keyFileEdit->setPlaceholderText("e.g. /home/timo/.ssh/id_ed25519 (optional)");

    auto *browseBtn = new QToolButton(rightWidget);
    browseBtn->setText("...");

    // Key file row: line edit + browse button
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

    // Form rows
    form->addRow("Name:", m_nameEdit);
    form->addRow("Group:", m_groupCombo); // NEW
    form->addRow("User:", m_userEdit);
    form->addRow("Host:", m_hostEdit);
    form->addRow("Port:", m_portSpin);
    form->addRow("", m_pqDebugCheck);
    form->addRow("Color scheme:", m_colorSchemeCombo);
    form->addRow("Font size:", m_fontSizeSpin);
    form->addRow("Window width:", m_widthSpin);
    form->addRow("Window height:", m_heightSpin);
    form->addRow("Scrollback lines:", m_historySpin);
    form->addRow("Key type:", m_keyTypeCombo);
    form->addRow("Key file:", keyRow);

    rightLayout->addLayout(form);

    // Save/Cancel
    m_buttonsBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, rightWidget);
    rightLayout->addWidget(m_buttonsBox);

    // Put both sides into main layout
    mainLayout->addWidget(leftWidget, 1);
    mainLayout->addWidget(rightWidget, 2);

    // Wiring
    connect(m_list, &QListWidget::currentRowChanged,
            this, &ProfilesEditorDialog::onListRowChanged);

    // Name edits update list item title live
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

// -----------------------------
// Selection change handling
// -----------------------------
//
// We always “sync out” current form fields into the currently-selected profile BEFORE
// switching to another row. That way you don’t lose edits when clicking around.
void ProfilesEditorDialog::onListRowChanged(int row)
{
    syncFormToCurrent();   // commit UI -> m_working[m_currentRow]
    m_currentRow = row;
    loadProfileToForm(row); // load m_working[row] -> UI
}

// -----------------------------
// Load a profile into the form
// -----------------------------
void ProfilesEditorDialog::loadProfileToForm(int row)
{
    // Keep group dropdown populated from current working set
    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());

    if (row < 0 || row >= m_working.size()) {
        // Clear form for “no selection”
        m_nameEdit->clear();
        if (m_groupCombo) m_groupCombo->setCurrentText("Ungrouped");
        m_userEdit->clear();
        m_hostEdit->clear();
        m_portSpin->setValue(22);
        m_pqDebugCheck->setChecked(true);
        m_colorSchemeCombo->setCurrentText("WhiteOnBlack");
        m_fontSizeSpin->setValue(11);
        m_widthSpin->setValue(900);
        m_heightSpin->setValue(500);
        if (m_historySpin) m_historySpin->setValue(2000);

        if (m_keyTypeCombo) m_keyTypeCombo->setCurrentText("auto");
        if (m_keyFileEdit) m_keyFileEdit->clear();
        return;
    }

    const SshProfile &p = m_working[row];

    m_nameEdit->setText(p.name);

    if (m_groupCombo) {
        const QString g = normalizedGroup(p.group);
        const int gidx = m_groupCombo->findText(g, Qt::MatchFixedString);
        if (gidx >= 0) m_groupCombo->setCurrentIndex(gidx);
        else m_groupCombo->setCurrentText(g);
    }

    m_userEdit->setText(p.user);
    m_hostEdit->setText(p.host);
    m_portSpin->setValue(p.port);
    m_pqDebugCheck->setChecked(p.pqDebug);

    // Term scheme: try to select an installed scheme; otherwise keep text as-is
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

    if (m_historySpin)
        m_historySpin->setValue(p.historyLines >= 0 ? p.historyLines : 2000);

    // Key auth fields
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

// -----------------------------
// Sync current form fields -> current profile in m_working
// -----------------------------
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

    if (m_groupCombo) {
        const QString g = m_groupCombo->currentText().trimmed();
        // Store empty as empty (so JSON stays clean), but treat empty as Ungrouped in UI/main list.
        p.group = g; // empty allowed
    }

    p.termColorScheme = m_colorSchemeCombo->currentText();
    p.termFontSize    = m_fontSizeSpin->value();

    p.termWidth  = m_widthSpin->value();
    p.termHeight = m_heightSpin->value();

    if (m_historySpin)
        p.historyLines = m_historySpin->value();

    if (m_keyTypeCombo) {
        const QString kt = m_keyTypeCombo->currentText().trimmed();
        p.keyType = kt.isEmpty() ? QString("auto") : kt;
    }
    if (m_keyFileEdit) {
        p.keyFile = m_keyFileEdit->text().trimmed();
    }

    // If name is blank, auto-generate a stable label
    if (p.name.isEmpty())
        p.name = QString("%1@%2").arg(p.user, p.host);

    // Keep list item in sync with current profile name
    if (QListWidgetItem *it = m_list->item(m_currentRow))
        it->setText(p.name);

    // Keep group combo updated as user creates new groups
    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());
}

// Live update list label when user edits Name: field.
// If Name is empty, show "user@host" as the visible label.
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

// -----------------------------
// Add / delete profile
// -----------------------------
void ProfilesEditorDialog::addProfile()
{
    // New profile default values.
    // Note: defaults should match ProfileStore::defaults() for consistency.
    SshProfile p;
    p.user = qEnvironmentVariable("USER", "user");
    p.host = "localhost";
    p.port = 22;
    p.pqDebug = true;

    p.group = ""; // empty => Ungrouped

    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize = 11;
    p.termWidth = 900;
    p.termHeight = 500;

    p.historyLines = 2000;

    p.keyFile = "";
    p.keyType = "auto";

    p.name = QString("%1@%2").arg(p.user, p.host);

    m_working.push_back(p);
    m_list->addItem(p.name);

    // Refresh group list (includes Ungrouped)
    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());

    // Select the new profile so user can edit immediately
    const int row = m_working.size() - 1;
    m_list->setCurrentRow(row);
}

void ProfilesEditorDialog::deleteProfile()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_working.size())
        return;

    m_working.remove(row);
    delete m_list->takeItem(row);

    // Refresh group list after deletion
    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());

    if (m_working.isEmpty()) {
        m_currentRow = -1;
        loadProfileToForm(-1);
        return;
    }

    // After deletion, select the closest remaining row
    const int newRow = qMin(row, m_working.size() - 1);
    m_list->setCurrentRow(newRow);
}

// -----------------------------
// Validation + accept
// -----------------------------
//
// This is the “gate” that prevents corrupt profiles from being committed.
bool ProfilesEditorDialog::validateProfiles(QString *errMsg) const
{
    for (const auto &p : m_working) {
        if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
            if (errMsg) *errMsg = "Each profile must have non-empty user and host.";
            return false;
        }

        // Optional sanity: if key type != auto and key file is empty -> warn
        // (Key type might become meaningful later. For now this just prevents a confusing config.)
        const QString kt = p.keyType.trimmed();
        if (!kt.isEmpty() && kt != "auto") {
            if (p.keyFile.trimmed().isEmpty()) {
                if (errMsg) *errMsg =
                    "Key type is set but key file is empty. Either set a key file or set key type to auto.";
                return false;
            }
        }
    }
    if (errMsg) errMsg->clear();
    return true;
}

void ProfilesEditorDialog::onAccepted()
{
    // Make sure current form edits are captured before validating/saving.
    syncFormToCurrent();

    QString err;
    if (!validateProfiles(&err)) {
        QMessageBox::warning(this, "Invalid profile", err);
        return;
    }

    // Commit working set to result and close dialog.
    m_result = m_working;
    accept();
}
