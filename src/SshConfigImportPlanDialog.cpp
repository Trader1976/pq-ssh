// SshConfigImportPlanDialog.cpp
//
// PURPOSE
// -------
// Presents a previewable, filterable “import plan” for converting parsed OpenSSH
// config entries (~/.ssh/config) into PQ-SSH profiles.
//
// The dialog does NOT parse ~/.ssh/config itself and does NOT directly apply changes.
// It is a UI orchestration layer that:
//  - Displays what would be imported/updated/skipped/invalid
//  - Lets user select which Create/Update actions to apply
//  - Emits applyRequested(creates, updates) for the caller to execute the write/update
//
// ARCHITECTURAL ROLE
// ------------------
// This dialog sits between:
//  - SshConfigParser (or similar) → produces SshConfigParseResult (already parsed)
//  - SshConfigImportPlan::buildPlan(...) → produces a list of import rows with actions
//  - Profile storage layer (caller) → actually creates/updates PQ-SSH profiles
//
// It intentionally maintains a clear boundary:
// - It must NOT mutate ~/.ssh/config.
// - It should treat ~/.ssh/config as an input source only.
// - It should not write PQ-SSH profiles directly; it emits a plan and delegates execution.
//
// UX PRINCIPLES
// -------------
// - All actions are reversible from a UX point of view (no silent writes).
// - "Apply" is disabled until at least one Create/Update row is selected.
// - Filtering is purely visual (hides rows) and does not change the underlying plan.
//
// STATE MODEL
// -----------
// - m_rows is the authoritative model (action + profile + reason + selected)
// - QTableWidget is the view
// - When the user checks/unchecks a row, we update m_rows[row].selected
// - rebuildPlan() regenerates m_rows from source parse + options (may reset selection)
//
// PERFORMANCE NOTE
// ----------------
// This uses QTableWidget for simplicity. For very large configs, a model/view approach
// (QAbstractTableModel) would be more scalable and less error-prone.

#include "SshConfigImportPlanDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QFileInfo>

static QString toLowerStr(const QString& s) { return s.toLower(); }

SshConfigImportPlanDialog::SshConfigImportPlanDialog(const QString& sourcePath,
                                                     const SshConfigParseResult& parsed,
                                                     const QStringList& existingProfileNames,
                                                     QWidget* parent)
    : QDialog(parent),
      m_path(sourcePath),
      m_parsed(parsed),
      m_existingNames(existingProfileNames)
{
    // Non-modal by design: user can keep it open while checking other UI if needed.
    // (If you find users accidentally interacting with the main window, consider modal.)
    setModal(false);

    setWindowTitle("CPUNK PQ-SSH — Import Plan");
    resize(980, 560);

    buildUi();
    rebuildPlan();
}

void SshConfigImportPlanDialog::buildUi()
{
    // Top-level layout
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    // Title shows the source file path (selectable for copying)
    m_title = new QLabel(this);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_title);

    // Summary shows action counts + selection counts and is updated as the user selects rows
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);

    // =========================
    // Options row
    // =========================
    // These options control plan generation (model rebuild), not just filtering.
    auto* optRow = new QHBoxLayout();

    // Apply GLOBAL defaults (OpenSSH "Host *" or global config) into each host entry
    // before deriving per-host profiles.
    m_applyGlobal = new QCheckBox("Apply GLOBAL defaults", this);
    m_applyGlobal->setChecked(true);

    // Skip wildcard host patterns like "Host *.corp" or "Host *"
    // (These are often not suitable for a concrete PQ-SSH profile.)
    m_skipWildcards = new QCheckBox("Skip wildcards", this);
    m_skipWildcards->setChecked(true);

    // If enabled: allow matching an existing PQ-SSH profile name and updating it.
    // If disabled: collisions turn into Skip/Invalid (depending on plan logic).
    m_allowUpdates = new QCheckBox("Allow updates", this);
    m_allowUpdates->setChecked(false);

    // Normalize identity file paths so "~" is used or resolved consistently.
    // This prevents importing absolute paths that are machine-specific.
    m_normPaths = new QCheckBox("Normalize IdentityFile paths (~)", this);
    m_normPaths->setChecked(true);

    optRow->addWidget(m_applyGlobal);
    optRow->addWidget(m_skipWildcards);
    optRow->addWidget(m_allowUpdates);
    optRow->addWidget(m_normPaths);
    optRow->addStretch(1);
    outer->addLayout(optRow);

    // Any option change regenerates the plan (m_rows) and then repopulates table.
    connect(m_applyGlobal, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_skipWildcards, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_allowUpdates, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_normPaths, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);

    // =========================
    // Filter row (view-only)
    // =========================
    // Filtering hides rows in the table but does not mutate m_rows.
    auto* filterRow = new QHBoxLayout();

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText("Filter (name / host / user) …");

    m_statusFilter = new QComboBox(this);
    m_statusFilter->addItem("All");
    m_statusFilter->addItem("Create");
    m_statusFilter->addItem("Update");
    m_statusFilter->addItem("Skip");
    m_statusFilter->addItem("Invalid");

    filterRow->addWidget(new QLabel("Filter:", this));
    filterRow->addWidget(m_filterEdit, 1);
    filterRow->addWidget(new QLabel("Status:", this));
    filterRow->addWidget(m_statusFilter);
    outer->addLayout(filterRow);

    connect(m_filterEdit, &QLineEdit::textChanged, this, &SshConfigImportPlanDialog::onFilterChanged);
    connect(m_statusFilter, &QComboBox::currentTextChanged, this, &SshConfigImportPlanDialog::onFilterChanged);

    // =========================
    // Plan table
    // =========================
    // Columns:
    //  0 Action: Create/Update/Skip/Invalid
    //  1 Select: checkbox (enabled only for Create/Update)
    //  2..6 Imported profile fields
    //  7 Reason: why this action was chosen (diagnostics)
    //
    // NOTE: QTableWidget is an imperative “view + data” widget.
    // We treat m_rows as the model and reflect selection changes back into it.
    m_table = new QTableWidget(this);
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels({
        "Action",
        "Select",
        "Name",
        "HostName",
        "User",
        "Port",
        "IdentityFile",
        "Reason"
    });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outer->addWidget(m_table, 1);

    // Keep selection state synchronized with m_rows.
    // We only react to column 1 (checkbox column).
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!it) return;
        if (it->column() != 1) return;

        const int row = it->row();
        if (row < 0 || row >= m_rows.size()) return;

        // Only Create/Update rows are selectable. Skip/Invalid rows are always unselected.
        const bool selectable =
            (m_rows[row].action == ImportAction::Create || m_rows[row].action == ImportAction::Update);
        if (!selectable) return;

        m_rows[row].selected = (it->checkState() == Qt::Checked);
        updateSummary();
    });

    // =========================
    // Buttons row
    // =========================
    auto* btnRow = new QHBoxLayout();

    // Refresh regenerates the plan from the already-parsed config (m_parsed).
    // If you want to re-parse the file itself, that should happen outside this dialog,
    // and a new dialog instance can be created with fresh SshConfigParseResult.
    m_refreshBtn = new QPushButton("Refresh", this);

    // Convenience: select all Create actions (safe default for most imports).
    m_selectCreatesBtn = new QPushButton("Select all Creates", this);

    // Apply triggers a confirmation and emits applyRequested(...) for the caller.
    m_applyBtn = new QPushButton("Apply", this);

    // "Save" is currently wired to the same onApply() as Apply.
    // If you later add “export plan to JSON/CSV”, wire it separately.
    auto* saveBtn = new QPushButton("Save", this);

    m_closeBtn = new QPushButton("Close", this);

    btnRow->addWidget(m_refreshBtn);
    btnRow->addWidget(m_selectCreatesBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_applyBtn);
    btnRow->addWidget(saveBtn);
    btnRow->addWidget(m_closeBtn);
    outer->addLayout(btnRow);

    connect(m_refreshBtn, &QPushButton::clicked, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_selectCreatesBtn, &QPushButton::clicked, this, [this]() { onToggleSelectAllCreates(true); });
    connect(m_applyBtn, &QPushButton::clicked, this, &SshConfigImportPlanDialog::onApply);
    connect(saveBtn, &QPushButton::clicked, this, &SshConfigImportPlanDialog::onApply);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

QString SshConfigImportPlanDialog::actionText(ImportAction a) const
{
    // Text mapping for the “Action” column. Must match status filter labels.
    switch (a) {
        case ImportAction::Create: return "Create";
        case ImportAction::Update: return "Update";
        case ImportAction::Skip:   return "Skip";
        case ImportAction::Invalid:return "Invalid";
    }
    return "Invalid";
}

void SshConfigImportPlanDialog::rebuildPlan()
{
    // Rebuilds the plan model (m_rows) from:
    // - parsed SSH config (m_parsed)
    // - list of existing PQ-SSH profile names (m_existingNames)
    // - current user options (m_opt)
    //
    // This method is the "single source of truth" for regenerating the plan.
    // It updates title/summary/view and re-applies any active filters.

    QFileInfo fi(m_path);
    m_title->setText(QString("Source: %1").arg(fi.absoluteFilePath()));

    // Capture current UI options into the plan options struct
    m_opt.applyGlobalDefaults = m_applyGlobal->isChecked();
    m_opt.skipWildcards = m_skipWildcards->isChecked();
    m_opt.allowUpdates = m_allowUpdates->isChecked();
    m_opt.normalizeIdentityPath = m_normPaths->isChecked();

    // Generate plan rows (model)
    m_rows = SshConfigImportPlan::buildPlan(m_parsed, m_existingNames, m_opt);

    // UX: if there is nothing to import, disable apply.
    // (Also: show a helpful hint to the user.)
    if (m_rows.isEmpty()) {
        m_summary->setText("No importable Host entries found. Add at least one 'Host name' block to ~/.ssh/config, then Refresh.");
        m_applyBtn->setEnabled(false);
    }

    // Re-render view and update derived UI state
    populateTable();
    updateSummary();
    applyFiltersToRows();
}

void SshConfigImportPlanDialog::populateTable()
{
    // Populates QTableWidget from m_rows.
    //
    // Important: QTableWidget emits itemChanged while being filled. We block signals
    // during the fill so our itemChanged handler doesn't interpret initialization as
    // user interaction.

    m_table->blockSignals(true);

    m_table->setRowCount(0);

    for (int i = 0; i < m_rows.size(); ++i) {
        const auto& r = m_rows[i];

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        // Column 0: Action text (read-only)
        auto* actItem = new QTableWidgetItem(actionText(r.action));
        actItem->setFlags(actItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 0, actItem);

        // Column 1: Select checkbox
        //
        // Only Create/Update rows can be selected; Skip/Invalid show disabled checkbox.
        auto* selItem = new QTableWidgetItem();
        const bool selectable = (r.action == ImportAction::Create || r.action == ImportAction::Update);

        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (selectable)
            flags |= Qt::ItemIsEnabled;
        selItem->setFlags(flags);

        selItem->setCheckState((selectable && r.selected) ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, 1, selItem);

        // Columns 2..7: Profile fields + reason
        m_table->setItem(row, 2, new QTableWidgetItem(r.profile.name));
        m_table->setItem(row, 3, new QTableWidgetItem(r.profile.hostName));
        m_table->setItem(row, 4, new QTableWidgetItem(r.profile.user));
        m_table->setItem(row, 5, new QTableWidgetItem(QString::number(r.profile.port)));
        m_table->setItem(row, 6, new QTableWidgetItem(r.profile.identityFile));
        m_table->setItem(row, 7, new QTableWidgetItem(r.reason));

        // Make all text cells read-only (defensive; edit triggers are already disabled)
        for (int c = 2; c < 8; ++c) {
            if (auto* it = m_table->item(row, c))
                it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        }
    }

    m_table->blockSignals(false);
    m_table->resizeColumnsToContents();
}

void SshConfigImportPlanDialog::updateSummary()
{
    // Computes counts from the model (m_rows) and updates the summary label and Apply enablement.
    // This is invoked after plan rebuild and after any selection toggle.

    const int cCreate  = SshConfigImportPlan::countAction(m_rows, ImportAction::Create);
    const int cUpdate  = SshConfigImportPlan::countAction(m_rows, ImportAction::Update);
    const int cSkip    = SshConfigImportPlan::countAction(m_rows, ImportAction::Skip);
    const int cInvalid = SshConfigImportPlan::countAction(m_rows, ImportAction::Invalid);

    const int selCreates = SshConfigImportPlan::selectedCreates(m_rows).size();
    const int selUpdates = SshConfigImportPlan::selectedUpdates(m_rows).size();

    m_summary->setText(QString("Plan: Create %1 (selected %2), Update %3 (selected %4), Skip %5, Invalid %6.")
                       .arg(cCreate).arg(selCreates)
                       .arg(cUpdate).arg(selUpdates)
                       .arg(cSkip)
                       .arg(cInvalid));

    // Apply only when something actionable is selected.
    m_applyBtn->setEnabled((selCreates + selUpdates) > 0);
}

void SshConfigImportPlanDialog::onFilterChanged()
{
    // Filter controls are view-only; apply them to hide/unhide table rows.
    applyFiltersToRows();
}

void SshConfigImportPlanDialog::applyFiltersToRows()
{
    // Applies textual filter and action-status filter to the view by hiding rows.
    //
    // Text filter is case-insensitive and matches against:
    // - profile name
    // - host
    // - user
    //
    // Status filter matches the rendered "Action" text, so it must stay consistent
    // with actionText() and combobox labels.

    const QString f  = toLowerStr(m_filterEdit->text().trimmed());
    const QString st = m_statusFilter->currentText();

    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString act  = m_table->item(row, 0)->text();
        const QString name = m_table->item(row, 2)->text();
        const QString host = m_table->item(row, 3)->text();
        const QString user = m_table->item(row, 4)->text();

        const bool okStatus = (st == "All") || (act == st);

        bool okText = true;
        if (!f.isEmpty()) {
            const QString hay = toLowerStr(name + " " + host + " " + user);
            okText = hay.contains(f);
        }

        m_table->setRowHidden(row, !(okStatus && okText));
    }
}

void SshConfigImportPlanDialog::onToggleSelectAllCreates(bool on)
{
    // Convenience action: bulk-select all Create rows.
    //
    // Note: This mutates the model (m_rows) and then reflects it into the view.
    // It does not affect Update rows because updates are higher risk and should be explicit.

    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows[row].action != ImportAction::Create) continue;

        m_rows[row].selected = on;

        auto* it = m_table->item(row, 1);
        if (it && (it->flags() & Qt::ItemIsEnabled)) {
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateSummary();
}

void SshConfigImportPlanDialog::onApply()
{
    // Collects selected actions from the plan model and asks for confirmation.
    // If confirmed, emits applyRequested(...) and closes the dialog.

    const auto creates = SshConfigImportPlan::selectedCreates(m_rows);
    const auto updates = SshConfigImportPlan::selectedUpdates(m_rows);

    if (creates.isEmpty() && updates.isEmpty()) {
        QMessageBox::information(this, "Import Plan", "Nothing selected to apply.");
        return;
    }

    // High-friction confirmation: importing updates can overwrite existing profile settings.
    // Note: This modifies PQ-SSH profiles only; ~/.ssh/config is never changed.
    const auto ans = QMessageBox::question(
        this,
        "Apply import",
        QString("Apply this plan?\n\nCreate: %1\nUpdate: %2\n\nThis will modify PQ-SSH profiles (not ~/.ssh/config).")
            .arg(creates.size()).arg(updates.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (ans != QMessageBox::Yes) return;

    // Delegate actual persistence to the owner/controller via signal.
    emit applyRequested(creates, updates);
    close();
}