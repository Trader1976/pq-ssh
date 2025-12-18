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
#include <QSignalBlocker>
#include <QSplitter>


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

bool ProfilesEditorDialog::isMacroEmpty(const ProfileMacro& m) const
{
    return m.shortcut.trimmed().isEmpty() && m.command.trimmed().isEmpty();
}

QString ProfilesEditorDialog::macroDisplayName(const ProfileMacro& m, int idx) const
{
    const QString nm = m.name.trimmed();
    if (!nm.isEmpty())
        return nm;

    const QString sc = m.shortcut.trimmed();
    if (!sc.isEmpty())
        return QString("%1 — %2").arg(idx + 1).arg(sc);

    const QString cmd = m.command.trimmed();
    if (!cmd.isEmpty()) {
        QString shortCmd = cmd;
        if (shortCmd.size() > 28) shortCmd = shortCmd.left(28) + "…";
        return QString("%1 — %2").arg(idx + 1).arg(shortCmd);
    }

    return QString("Macro %1").arg(idx + 1);
}

void ProfilesEditorDialog::rebuildMacroList()
{
    if (!m_macroList) return;

    QSignalBlocker b(m_macroList);
    m_macroList->clear();

    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    const auto &macros = m_working[m_currentRow].macros;
    for (int i = 0; i < macros.size(); ++i) {
        m_macroList->addItem(macroDisplayName(macros[i], i));
    }
}


void ProfilesEditorDialog::ensureMacroSelectionValid()
{
    if (!m_macroList) return;

    if (m_currentRow < 0 || m_currentRow >= m_working.size()) {
        m_macroList->setCurrentRow(-1);
        return;
    }

    const int count = m_macroList->count();
    if (count <= 0) {
        m_macroList->setCurrentRow(-1);
        return;
    }

    int r = m_macroList->currentRow();
    if (r < 0) r = 0;
    if (r >= count) r = count - 1;
    m_macroList->setCurrentRow(r);
}
void ProfilesEditorDialog::loadMacroToEditor(int macroRow)
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    const auto &macros = m_working[m_currentRow].macros;
    if (macroRow < 0 || macroRow >= macros.size())
        return;

    m_currentMacroRow = macroRow;
    const ProfileMacro &m = macros[macroRow];

    // Block signals so we don't re-trigger updates while loading
    QSignalBlocker b1(m_macroNameEdit);
    QSignalBlocker b2(m_macroShortcutEdit);
    QSignalBlocker b3(m_macroCmdEdit);
    QSignalBlocker b4(m_macroEnterCheck);

    if (m_macroNameEdit) m_macroNameEdit->setText(m.name);
    if (m_macroShortcutEdit) {
        const QString sc = m.shortcut.trimmed();
        m_macroShortcutEdit->setKeySequence(sc.isEmpty() ? QKeySequence() : QKeySequence(sc));
    }
    if (m_macroCmdEdit) m_macroCmdEdit->setText(m.command);
    if (m_macroEnterCheck) m_macroEnterCheck->setChecked(m.sendEnter);
}

// Writes the currently visible macro editor fields -> m_working[m_currentRow].macros[m_currentMacroRow]
void ProfilesEditorDialog::syncMacroEditorToCurrent()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    SshProfile &p = m_working[m_currentRow];

    if (m_currentMacroRow < 0 || m_currentMacroRow >= p.macros.size())
        return;

    ProfileMacro &m = p.macros[m_currentMacroRow];

    // Name
    if (m_macroNameEdit)
        m.name = m_macroNameEdit->text().trimmed();

    // Shortcut
    if (m_macroShortcutEdit)
        m.shortcut = m_macroShortcutEdit->keySequence().toString().trimmed();

    // Command
    if (m_macroCmdEdit)
        m.command = m_macroCmdEdit->text();

    // Enter behavior
    if (m_macroEnterCheck)
        m.sendEnter = m_macroEnterCheck->isChecked();

    // Update list item label live (no full rebuild needed)
    if (m_macroList && m_currentMacroRow >= 0 && m_currentMacroRow < m_macroList->count()) {
        if (QListWidgetItem *it = m_macroList->item(m_currentMacroRow)) {
            it->setText(macroDisplayName(m, m_currentMacroRow));
        }
    }
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

    // =========================================================
    // Column 1: profile list + Add/Delete
    // =========================================================
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

    // =========================================================
    // Column 2: profile details form
    // =========================================================
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
    if (m_groupCombo->lineEdit())
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
        if (!path.isEmpty() && m_keyFileEdit)
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

    m_buttonsBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, detailsWidget);
    detailsLayout->addWidget(m_buttonsBox);

    // =========================================================
    // Column 3: Macros (multi)
    // =========================================================
    auto *macroOuter = new QWidget(split);
    auto *macroOuterL = new QVBoxLayout(macroOuter);
    macroOuterL->setContentsMargins(0, 0, 0, 0);
    macroOuterL->setSpacing(8);

    auto *macroPanel = new QWidget(macroOuter);
    macroPanel->setObjectName("profileMacroPanel");
    macroPanel->setStyleSheet(
        "#profileMacroPanel {"
        "  border: 1px solid #2a2a2a;"
        "  border-radius: 8px;"
        "}"
    );
    macroPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *macroL = new QVBoxLayout(macroPanel);
    macroL->setContentsMargins(10, 10, 10, 10);
    macroL->setSpacing(8);

    auto *macroTitle = new QLabel("Hotkey macros", macroPanel);
    macroTitle->setStyleSheet("font-weight: bold;");

    // list + buttons row
    auto *macroListRow = new QWidget(macroPanel);
    auto *macroListRowL = new QHBoxLayout(macroListRow);
    macroListRowL->setContentsMargins(0, 0, 0, 0);
    macroListRowL->setSpacing(6);

    m_macroList = new QListWidget(macroPanel);
    m_macroList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_macroList->setMinimumHeight(160);

    auto *macroBtnsCol = new QWidget(macroPanel);
    auto *macroBtnsColL = new QVBoxLayout(macroBtnsCol);
    macroBtnsColL->setContentsMargins(0, 0, 0, 0);
    macroBtnsColL->setSpacing(6);

    m_macroAddBtn = new QPushButton("+", macroBtnsCol);
    m_macroAddBtn->setToolTip("Add macro");

    m_macroDelBtn = new QPushButton("-", macroBtnsCol);
    m_macroDelBtn->setToolTip("Delete selected macro");

    macroBtnsColL->addWidget(m_macroAddBtn);
    macroBtnsColL->addWidget(m_macroDelBtn);
    macroBtnsColL->addStretch(1);

    macroListRowL->addWidget(m_macroList, 1);
    macroListRowL->addWidget(macroBtnsCol, 0);

    // Macro editor: Name
    auto *nameLbl = new QLabel("Name:", macroPanel);
    m_macroNameEdit = new QLineEdit(macroPanel);
    m_macroNameEdit->setPlaceholderText("e.g. Backup stats");

    // Macro editor: Shortcut + Clear + Command in one row
    auto *rowLbls = new QWidget(macroPanel);
    auto *rowLblsL = new QHBoxLayout(rowLbls);
    rowLblsL->setContentsMargins(0, 0, 0, 0);
    rowLblsL->setSpacing(6);

    auto *shortcutLbl = new QLabel("Shortcut:", macroPanel);
    auto *cmdLbl      = new QLabel("Command:",  macroPanel);

    shortcutLbl->setMinimumWidth(70);
    rowLblsL->addWidget(shortcutLbl, 0);

    // spacer that matches shortcut + clear widths so "Command" label aligns
    auto *lblSpacer = new QWidget(macroPanel);
    lblSpacer->setFixedWidth(100 + 56 + 6); // shortcut(100) + clear(56) + spacing
    rowLblsL->addWidget(lblSpacer, 0);

    rowLblsL->addWidget(cmdLbl, 1);

    auto *macroRow = new QWidget(macroPanel);
    auto *macroRowL = new QHBoxLayout(macroRow);
    macroRowL->setContentsMargins(0, 0, 0, 0);
    macroRowL->setSpacing(6);

    m_macroShortcutEdit = new QKeySequenceEdit(macroPanel);
    m_macroShortcutEdit->setToolTip("Click and press a shortcut, e.g. F2, Alt+X, Ctrl+Shift+R");
    m_macroShortcutEdit->setMinimumWidth(90);
    m_macroShortcutEdit->setMaximumWidth(120);
    m_macroShortcutEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_macroClearBtn = new QPushButton("Clear", macroPanel);
    m_macroClearBtn->setToolTip("Clear the shortcut");
    m_macroClearBtn->setFixedWidth(56);

    m_macroCmdEdit = new QLineEdit(macroPanel);
    m_macroCmdEdit->setPlaceholderText(R"(e.g. cd stats && cp stats.txt stats_backup.txt)");
    m_macroCmdEdit->setToolTip("Command to send when the shortcut is pressed");
    m_macroCmdEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    macroRowL->addWidget(m_macroShortcutEdit, 0);
    macroRowL->addWidget(m_macroClearBtn, 0);
    macroRowL->addWidget(m_macroCmdEdit, 1); // <-- stretches full width

    m_macroEnterCheck = new QCheckBox("Send [Enter] automatically after command", macroPanel);
    m_macroEnterCheck->setChecked(true);

    auto *macroHint = new QLabel(
        "Tip: If [Enter] is enabled, PQ-SSH appends a newline so the command runs immediately.",
        macroPanel
    );
    macroHint->setWordWrap(true);
    macroHint->setStyleSheet("color: #9aa0a6; font-size: 12px;");

    // assemble macro panel
    macroL->addWidget(macroTitle);
    macroL->addWidget(macroListRow, 0);
    macroL->addSpacing(6);
    macroL->addWidget(nameLbl);
    macroL->addWidget(m_macroNameEdit);
    macroL->addWidget(rowLbls);
    macroL->addWidget(macroRow);
    macroL->addWidget(m_macroEnterCheck);
    macroL->addWidget(macroHint);
    macroL->addStretch(1);

    macroOuterL->addWidget(macroPanel, 1);

    // =========================================================
    // Splitter sizes
    // =========================================================
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 5);
    split->setSizes({260, 380, 620});

    // =========================================================
    // Wiring
    // =========================================================
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

    // Macros wiring
    connect(m_macroList, &QListWidget::currentRowChanged,
            this, &ProfilesEditorDialog::onMacroRowChanged);

    connect(m_macroAddBtn, &QPushButton::clicked,
            this, &ProfilesEditorDialog::addMacro);

    connect(m_macroDelBtn, &QPushButton::clicked,
            this, &ProfilesEditorDialog::deleteMacro);

    connect(m_macroClearBtn, &QPushButton::clicked,
            this, &ProfilesEditorDialog::clearMacroShortcut);

    connect(m_macroNameEdit, &QLineEdit::textChanged,
            this, &ProfilesEditorDialog::onMacroNameEdited);

    // If profile has no macros, start with an empty one so UI is usable immediately
    if (!m_working.isEmpty() && m_working[0].macros.isEmpty()) {
        ProfileMacro m;
        m.name = "";
        m.shortcut = "";
        m.command = "";
        m.sendEnter = true;
        m_working[0].macros.push_back(m);
    }

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
    // 1) Save current UI -> current profile (including currently selected macro)
    syncFormToCurrent();

    // 2) Load newly selected profile -> UI
    loadProfileToForm(row);
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
        populateGroupCombo(m_groupCombo, m_working, p.group);
        const QString g = p.group.trimmed();
        if (g.isEmpty()) m_groupCombo->setCurrentText("");
        else             m_groupCombo->setCurrentText(g);
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
        else          m_colorSchemeCombo->setCurrentText(scheme);
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
            m_keyTypeCombo->addItem(kt);
            m_keyTypeCombo->setCurrentIndex(m_keyTypeCombo->count() - 1);
        }
    }

    if (m_keyFileEdit) m_keyFileEdit->setText(p.keyFile);

    // -------------------------
    // Hotkey macros (multi)
    // -------------------------
    // Ensure profile always has at least one macro row so the editor is usable.
    if (m_working[row].macros.isEmpty()) {
        ProfileMacro m;
        m.name = "";
        m.shortcut = "";
        m.command = "";
        m.sendEnter = true;
        m_working[row].macros.push_back(m);
    }

    // Rebuild list and select a macro (prefer previous selection if valid).
    rebuildMacroList();

    int wantRow = m_currentMacroRow;
    if (wantRow < 0) wantRow = 0;
    if (m_macroList && wantRow >= m_macroList->count())
        wantRow = m_macroList->count() - 1;

    if (m_macroList) {
        QSignalBlocker b(m_macroList);
        m_macroList->setCurrentRow(wantRow);
    }

    // Load selected macro into editor (blocks signals inside loadMacroToEditor()).
    loadMacroToEditor(wantRow);

    // If for some reason list is missing, still clear editor safely.
    if (!m_macroList) {
        if (m_macroNameEdit) m_macroNameEdit->clear();
        if (m_macroShortcutEdit) m_macroShortcutEdit->setKeySequence(QKeySequence());
        if (m_macroCmdEdit) m_macroCmdEdit->clear();
        if (m_macroEnterCheck) m_macroEnterCheck->setChecked(true);
        m_currentMacroRow = -1;
    }
}

// -----------------------------
// Sync current form fields -> current profile in m_working
// -----------------------------
void ProfilesEditorDialog::syncFormToCurrent()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    // Save macro editor -> current macro
    syncMacroEditorToCurrent();

    SshProfile &p = m_working[m_currentRow];

    // Core connection
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

    // Terminal appearance
    if (m_colorSchemeCombo)
        p.termColorScheme = m_colorSchemeCombo->currentText().trimmed();
    if (m_fontSizeSpin) p.termFontSize = m_fontSizeSpin->value();
    if (m_widthSpin)    p.termWidth    = m_widthSpin->value();
    if (m_heightSpin)   p.termHeight   = m_heightSpin->value();
    if (m_historySpin)  p.historyLines = m_historySpin->value();

    // Auth
    if (m_keyTypeCombo) p.keyType = m_keyTypeCombo->currentText().trimmed();
    if (p.keyType.isEmpty()) p.keyType = "auto";
    if (m_keyFileEdit) p.keyFile = m_keyFileEdit->text().trimmed();

    // Keep list label in sync
    const QString shownName =
        p.name.trimmed().isEmpty()
            ? QString("%1@%2").arg(p.user, p.host)
            : p.name.trimmed();

    if (m_list && m_currentRow >= 0 && m_currentRow < m_list->count()) {
        if (QListWidgetItem *it = m_list->item(m_currentRow))
            it->setText(shownName);
    }

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

    // Hotkey macros (multi) defaults: ensure UI is usable immediately
    p.macros.clear();
    {
        ProfileMacro m;
        m.name      = "";
        m.shortcut  = "";
        m.command   = "";
        m.sendEnter = true;
        p.macros.push_back(m);
    }

    // (Optional) if you still have legacy single-macro fields in SshProfile,
    // keep them consistent (harmless, but not used by the new UI).
    p.macroShortcut = "";
    p.macroCommand  = "";
    p.macroEnter    = true;

    // Display label fallback
    p.name = QString("%1@%2").arg(p.user, p.host);

    // Commit new profile
    m_working.push_back(p);
    if (m_list) m_list->addItem(p.name);

    // Refresh group list after addition
    if (m_groupCombo)
        populateGroupCombo(m_groupCombo, m_working, m_groupCombo->currentText());

    // Select the new profile row (this will load the UI via onListRowChanged)
    const int row = m_working.size() - 1;
    if (m_list) m_list->setCurrentRow(row);
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


void ProfilesEditorDialog::onMacroRowChanged(int row)
{
    // Save edits to previous macro first
    syncMacroEditorToCurrent();

    m_currentMacroRow = row;
    loadMacroToEditor(row);
}
void ProfilesEditorDialog::onMacroNameEdited(const QString & /*text*/)
{
    // Live list label update
    syncMacroEditorToCurrent();
}


void ProfilesEditorDialog::addMacro()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    // Save current macro edits first
    syncMacroEditorToCurrent();

    ProfileMacro m;
    m.name = "";
    m.shortcut = "";
    m.command = "";
    m.sendEnter = true;

    m_working[m_currentRow].macros.push_back(m);

    rebuildMacroList();
    ensureMacroSelectionValid();
    m_macroList->setCurrentRow(m_working[m_currentRow].macros.size() - 1);
}

void ProfilesEditorDialog::deleteMacro()
{
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return;

    const int mi = currentMacroIndex();
    if (mi < 0) return;

    auto &macros = m_working[m_currentRow].macros;
    if (mi >= macros.size()) return;

    macros.remove(mi);

    // Keep at least one macro row so UI stays usable (optional choice)
    if (macros.isEmpty()) {
        ProfileMacro m;
        m.sendEnter = true;
        macros.push_back(m);
    }

    rebuildMacroList();
    ensureMacroSelectionValid();
}

void ProfilesEditorDialog::clearMacroShortcut()
{
    if (!m_macroShortcutEdit) return;
    m_macroShortcutEdit->setKeySequence(QKeySequence());
    syncMacroEditorToCurrent();
}

int ProfilesEditorDialog::currentMacroIndex() const
{
    if (!m_macroList) return -1;

    const int row = m_macroList->currentRow();
    if (row < 0) return -1;

    // Ensure we have a valid profile selected
    if (m_currentRow < 0 || m_currentRow >= m_working.size())
        return -1;

    const auto &macros = m_working[m_currentRow].macros;
    if (row >= macros.size())
        return -1;

    return row;
}
