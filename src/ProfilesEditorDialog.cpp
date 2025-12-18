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
#include <QSplitter>
#include <QKeySequenceEdit>
#include <QKeySequence>


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
// Layout is a simple 3-column version:
// - Left: list of profiles + Add/Delete
// - Middle: form editing fields for selected profile
// - Righ: macros
void ProfilesEditorDialog::buildUi()
{
    // Use a splitter so we can have 3 columns + adjustable spacing
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // ---------- Column 1: profile list + Add/Delete ----------
    auto *leftWidget = new QWidget(split);
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
    buttonsLayout->addStretch(1);

    leftLayout->addWidget(listLabel);
    leftLayout->addWidget(m_list, 1);
    leftLayout->addWidget(buttonsRow, 0);

    // ---------- Column 2: profile details form ----------
    auto *detailsWidget = new QWidget(split);
    auto *detailsLayout = new QVBoxLayout(detailsWidget);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(6);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);

    m_nameEdit = new QLineEdit(detailsWidget);
    m_userEdit = new QLineEdit(detailsWidget);
    m_hostEdit = new QLineEdit(detailsWidget);

    m_portSpin = new QSpinBox(detailsWidget);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);

    m_groupCombo = new QComboBox(detailsWidget);
    m_groupCombo->setEditable(true);
    m_groupCombo->setInsertPolicy(QComboBox::NoInsert);
    m_groupCombo->setToolTip("Group name for sorting in main window (empty = Ungrouped)");
    m_groupCombo->lineEdit()->setPlaceholderText("Ungrouped");
    populateGroupCombo(m_groupCombo, m_working);

    m_pqDebugCheck = new QCheckBox("Enable PQ debug (-vv)", detailsWidget);

    m_colorSchemeCombo = new QComboBox(detailsWidget);
    fillSchemeCombo(m_colorSchemeCombo);

    m_fontSizeSpin = new QSpinBox(detailsWidget);
    m_fontSizeSpin->setRange(6, 32);
    m_fontSizeSpin->setValue(11);

    m_widthSpin = new QSpinBox(detailsWidget);
    m_widthSpin->setRange(400, 4000);
    m_widthSpin->setValue(900);

    m_heightSpin = new QSpinBox(detailsWidget);
    m_heightSpin->setRange(300, 3000);
    m_heightSpin->setValue(500);

    m_historySpin = new QSpinBox(detailsWidget);
    m_historySpin->setRange(0, 50000);
    m_historySpin->setSingleStep(500);
    m_historySpin->setValue(2000);
    m_historySpin->setToolTip("Terminal scrollback buffer lines (0 = unlimited)");

    m_keyTypeCombo = new QComboBox(detailsWidget);
    m_keyTypeCombo->addItem("auto");
    m_keyTypeCombo->addItem("openssh");
    m_keyTypeCombo->addItem("pq"); // placeholder

    m_keyFileEdit = new QLineEdit(detailsWidget);
    m_keyFileEdit->setPlaceholderText("e.g. /home/timo/.ssh/id_ed25519 (optional)");

    auto *browseBtn = new QToolButton(detailsWidget);
    browseBtn->setText("...");

    auto *keyRow = new QWidget(detailsWidget);
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
    form->addRow("Group:", m_groupCombo);
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

    detailsLayout->addLayout(form);

    // Save/Cancel stays under details column
    m_buttonsBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, detailsWidget);
    detailsLayout->addWidget(m_buttonsBox);

    // ---------- Column 3: Hotkey macro (optional) ----------
    auto *extraOuter = new QWidget(split);
    auto *extraOuterL = new QHBoxLayout(extraOuter);
    extraOuterL->setContentsMargins(0, 0, 0, 0);
    extraOuterL->setSpacing(10);

    auto *extraPanel = new QWidget(extraOuter);
    extraPanel->setObjectName("profileExtraPanel");
    extraPanel->setStyleSheet(
        "#profileExtraPanel {"
        "  border: 1px solid #2a2a2a;"
        "  border-radius: 8px;"
        "}"
    );

    auto *extraL = new QVBoxLayout(extraPanel);
    extraL->setContentsMargins(10, 10, 10, 10);
    extraL->setSpacing(8);

    auto *macroTitle = new QLabel("Hotkey macro (optional)", extraPanel);
    macroTitle->setStyleSheet("font-weight: bold;");

    // --- NEW layout: Shortcut (small) + Clear + Command on the same row ---
    auto *macroRowLabels = new QWidget(extraPanel);
    auto *macroRowLabelsL = new QHBoxLayout(macroRowLabels);
    macroRowLabelsL->setContentsMargins(0, 0, 0, 0);
    macroRowLabelsL->setSpacing(6);

    auto *shortcutLbl = new QLabel("Shortcut:", extraPanel);
    auto *cmdLbl      = new QLabel("Command:",  extraPanel);

    // We align the labels with their controls (shortcut label over the small edit,
    // command label over the command edit).
    shortcutLbl->setMinimumWidth(70);
    macroRowLabelsL->addWidget(shortcutLbl, 1);

    // spacer matching shortcut edit width + clear button width so "Command:" sits above command edit
    auto *labelsSpacer = new QWidget(extraPanel);
    labelsSpacer->setFixedWidth(160 + 56); // shortcut edit (~160) + clear btn (~56)
    macroRowLabelsL->addWidget(labelsSpacer, 0);

    macroRowLabelsL->addWidget(cmdLbl, 1);

    m_macroShortcutEdit = new QKeySequenceEdit(extraPanel);
    m_macroShortcutEdit->setToolTip("Click here and press a shortcut, e.g. F2, Alt+X, Ctrl+Shift+R");
    m_macroShortcutEdit->setMinimumWidth(90);
    m_macroShortcutEdit->setMaximumWidth(100);
    m_macroShortcutEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto *macroClearBtn = new QPushButton("Clear", extraPanel);
    macroClearBtn->setToolTip("Clear the shortcut");
    macroClearBtn->setFixedWidth(56);

    m_macroCmdEdit = new QLineEdit(extraPanel);
    m_macroCmdEdit->setPlaceholderText(R"(e.g. cd stats && cp stats.txt stats_backup.txt)");
    m_macroCmdEdit->setToolTip("Command to send when the shortcut is pressed (optional)");
    m_macroCmdEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *macroRow = new QWidget(extraPanel);
    auto *macroRowL = new QHBoxLayout(macroRow);
    macroRowL->setContentsMargins(0, 0, 0, 0);
    macroRowL->setSpacing(6);

    macroRowL->addWidget(m_macroShortcutEdit, 0);
    macroRowL->addWidget(macroClearBtn, 0);
    macroRowL->addWidget(m_macroCmdEdit, 1);

    connect(macroClearBtn, &QPushButton::clicked, this, [this]() {
        if (m_macroShortcutEdit)
            m_macroShortcutEdit->setKeySequence(QKeySequence());
    });

    m_macroEnterCheck = new QCheckBox("Send [Enter] automatically after command", extraPanel);
    m_macroEnterCheck->setChecked(true);

    auto *macroHint = new QLabel(
        "Tip: If [Enter] is enabled, PQ-SSH appends a newline so the command runs immediately.",
        extraPanel
    );
    macroHint->setWordWrap(true);
    macroHint->setStyleSheet("color: #9aa0a6; font-size: 12px;");

    extraL->addWidget(macroTitle);
    extraL->addWidget(macroRowLabels);
    extraL->addWidget(macroRow);
    extraL->addWidget(m_macroEnterCheck);
    extraL->addWidget(macroHint);
    extraL->addStretch(1);

    // Make the panel use all available width
    extraPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // No "air" spacer anymore — panel gets everything
    extraOuterL->addWidget(extraPanel, 1);

    // Stretch / initial sizes
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 5);
    split->setSizes({260, 360, 520});

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

    if (m_list->count() > 0 && m_list->currentRow() < 0)
        m_list->setCurrentRow(0);
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
    if (row < 0 || row >= m_working.size())
        return;

    m_currentRow = row;
    const SshProfile &p = m_working[row];

    // -------------------------
    // Core connection settings
    // -------------------------
    if (m_nameEdit) m_nameEdit->setText(p.name);
    if (m_userEdit) m_userEdit->setText(p.user);
    if (m_hostEdit) m_hostEdit->setText(p.host);
    if (m_portSpin) m_portSpin->setValue(p.port > 0 ? p.port : 22);

    // Group
    if (m_groupCombo) {
        // Keep combo populated and try to select current group text
        populateGroupCombo(m_groupCombo, m_working, p.group);
        const QString g = p.group.trimmed();
        if (g.isEmpty()) {
            m_groupCombo->setCurrentText("");
        } else {
            m_groupCombo->setCurrentText(g);
        }
    }

    // Debug
    if (m_pqDebugCheck) m_pqDebugCheck->setChecked(p.pqDebug);

    // -------------------------
    // Terminal appearance
    // -------------------------
    if (m_colorSchemeCombo) {
        const QString scheme = p.termColorScheme.trimmed().isEmpty()
            ? QStringLiteral("WhiteOnBlack")
            : p.termColorScheme.trimmed();
        const int idx = m_colorSchemeCombo->findText(scheme);
        if (idx >= 0) m_colorSchemeCombo->setCurrentIndex(idx);
        else m_colorSchemeCombo->setCurrentText(scheme);
    }

    if (m_fontSizeSpin) m_fontSizeSpin->setValue(p.termFontSize > 0 ? p.termFontSize : 11);
    if (m_widthSpin)    m_widthSpin->setValue(p.termWidth > 0 ? p.termWidth : 900);
    if (m_heightSpin)   m_heightSpin->setValue(p.termHeight > 0 ? p.termHeight : 500);
    if (m_historySpin)  m_historySpin->setValue(p.historyLines >= 0 ? p.historyLines : 2000);

    // -------------------------
    // Auth
    // -------------------------
    if (m_keyTypeCombo) {
        const QString kt = p.keyType.trimmed().isEmpty()
            ? QStringLiteral("auto")
            : p.keyType.trimmed();

        const int idx = m_keyTypeCombo->findText(kt);
        if (idx >= 0) m_keyTypeCombo->setCurrentIndex(idx);
        else {
            // Keep it visible even if combo didn't contain it
            m_keyTypeCombo->addItem(kt);
            m_keyTypeCombo->setCurrentIndex(m_keyTypeCombo->count() - 1);
        }
    }

    if (m_keyFileEdit) m_keyFileEdit->setText(p.keyFile);

    // -------------------------
    // Hotkey macro (single)
    // -------------------------
    if (m_macroShortcutEdit) {
        const QString sc = p.macroShortcut.trimmed();
        m_macroShortcutEdit->setKeySequence(sc.isEmpty() ? QKeySequence()
                                                         : QKeySequence(sc));
    }

    if (m_macroCmdEdit)
        m_macroCmdEdit->setText(p.macroCommand);

    if (m_macroEnterCheck)
        m_macroEnterCheck->setChecked(p.macroEnter);
}

// -----------------------------
// Sync current form fields -> current profile in m_working
// -----------------------------
void ProfilesEditorDialog::syncFormToCurrent()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    SshProfile &p = m_working[m_currentRow];

    // -------------------------
    // Core connection settings
    // -------------------------
    if (m_nameEdit) p.name = m_nameEdit->text().trimmed();
    if (m_userEdit) p.user = m_userEdit->text().trimmed();
    if (m_hostEdit) p.host = m_hostEdit->text().trimmed();
    if (m_portSpin) p.port = m_portSpin->value();

    // Group
    if (m_groupCombo)
        p.group = m_groupCombo->currentText().trimmed();

    // Debug
    if (m_pqDebugCheck)
        p.pqDebug = m_pqDebugCheck->isChecked();

    // -------------------------
    // Terminal appearance
    // -------------------------
    if (m_colorSchemeCombo)
        p.termColorScheme = m_colorSchemeCombo->currentText().trimmed();

    if (m_fontSizeSpin) p.termFontSize = m_fontSizeSpin->value();
    if (m_widthSpin)    p.termWidth    = m_widthSpin->value();
    if (m_heightSpin)   p.termHeight   = m_heightSpin->value();
    if (m_historySpin)  p.historyLines = m_historySpin->value();

    // -------------------------
    // Auth
    // -------------------------
    if (m_keyTypeCombo) p.keyType = m_keyTypeCombo->currentText().trimmed();
    if (p.keyType.isEmpty()) p.keyType = "auto";

    if (m_keyFileEdit) p.keyFile = m_keyFileEdit->text().trimmed();

    // -------------------------
    // Hotkey macro (single)
    // -------------------------
    if (m_macroShortcutEdit) {
        const QString sc = m_macroShortcutEdit->keySequence().toString().trimmed();
        p.macroShortcut = sc;
    }

    if (m_macroCmdEdit)
        p.macroCommand = m_macroCmdEdit->text();

    if (m_macroEnterCheck)
        p.macroEnter = m_macroEnterCheck->isChecked();

    // -------------------------
    // Keep list label in sync (human readable)
    // -------------------------
    const QString shownName =
        p.name.trimmed().isEmpty()
            ? QString("%1@%2").arg(p.user, p.host)
            : p.name.trimmed();

    if (m_list && m_currentRow >= 0 && m_currentRow < m_list->count()) {
        QListWidgetItem *it = m_list->item(m_currentRow);
        if (it) it->setText(shownName);
    }

    // Also store back normalized display name
    p.name = shownName;
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
    // Keep in sync with ProfileStore::defaults() as much as possible.
    SshProfile p;

    p.user    = qEnvironmentVariable("USER", "user");
    p.host    = "localhost";
    p.port    = 22;
    p.pqDebug = true;

    p.group = ""; // empty => Ungrouped

    p.termColorScheme = "WhiteOnBlack";
    p.termFontSize    = 11;
    p.termWidth       = 900;
    p.termHeight      = 500;
    p.historyLines    = 2000;

    p.keyFile = "";
    p.keyType = "auto";

    // Hotkey macro (single) defaults
    p.macroShortcut = "";
    p.macroCommand  = "";
    p.macroEnter    = true;

    // Display label fallback
    p.name = QString("%1@%2").arg(p.user, p.host);

    m_working.push_back(p);
    m_list->addItem(p.name);

    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());

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
