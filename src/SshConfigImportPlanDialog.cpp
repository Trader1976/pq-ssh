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
    setModal(false);
    setWindowTitle("CPUNK PQ-SSH — Import Plan");
    resize(980, 560);

    buildUi();
    rebuildPlan();
}

void SshConfigImportPlanDialog::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_title);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);

    // Options row
    auto* optRow = new QHBoxLayout();
    m_applyGlobal = new QCheckBox("Apply GLOBAL defaults", this);
    m_applyGlobal->setChecked(true);

    m_skipWildcards = new QCheckBox("Skip wildcards", this);
    m_skipWildcards->setChecked(true);

    m_allowUpdates = new QCheckBox("Allow updates", this);
    m_allowUpdates->setChecked(false);

    m_normPaths = new QCheckBox("Normalize IdentityFile paths (~)", this);
    m_normPaths->setChecked(true);

    optRow->addWidget(m_applyGlobal);
    optRow->addWidget(m_skipWildcards);
    optRow->addWidget(m_allowUpdates);
    optRow->addWidget(m_normPaths);
    optRow->addStretch(1);
    outer->addLayout(optRow);

    connect(m_applyGlobal, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_skipWildcards, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_allowUpdates, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);
    connect(m_normPaths, &QCheckBox::toggled, this, &SshConfigImportPlanDialog::rebuildPlan);

    // Filter row
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

    // Table
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
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (!it) return;
        if (it->column() != 1) return;

        const int row = it->row();
        if (row < 0 || row >= m_rows.size()) return;

        const bool selectable =
            (m_rows[row].action == ImportAction::Create || m_rows[row].action == ImportAction::Update);
        if (!selectable) return;

        m_rows[row].selected = (it->checkState() == Qt::Checked);
        updateSummary();
    });
    // Buttons
    auto* btnRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton("Refresh", this);
    m_selectCreatesBtn = new QPushButton("Select all Creates", this);
    m_applyBtn = new QPushButton("Apply", this);
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
    QFileInfo fi(m_path);
    m_title->setText(QString("Source: %1").arg(fi.absoluteFilePath()));

    m_opt.applyGlobalDefaults = m_applyGlobal->isChecked();
    m_opt.skipWildcards = m_skipWildcards->isChecked();
    m_opt.allowUpdates = m_allowUpdates->isChecked();
    m_opt.normalizeIdentityPath = m_normPaths->isChecked();

    m_rows = SshConfigImportPlan::buildPlan(m_parsed, m_existingNames, m_opt);
    if (m_rows.isEmpty()) {
        m_summary->setText("No importable Host entries found. Add at least one 'Host name' block to ~/.ssh/config, then Refresh.");
        m_applyBtn->setEnabled(false);
    }

    populateTable();
    updateSummary();
    applyFiltersToRows();
}

void SshConfigImportPlanDialog::populateTable()
{
    // Avoid itemChanged firing while we fill the table
    m_table->blockSignals(true);

    m_table->setRowCount(0);

    for (int i = 0; i < m_rows.size(); ++i) {
        const auto& r = m_rows[i];

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        // Action
        auto* actItem = new QTableWidgetItem(actionText(r.action));
        actItem->setFlags(actItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 0, actItem);

        // Select checkbox
        auto* selItem = new QTableWidgetItem();
        const bool selectable = (r.action == ImportAction::Create || r.action == ImportAction::Update);

        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (selectable)
            flags |= Qt::ItemIsEnabled;
        selItem->setFlags(flags);

        selItem->setCheckState((selectable && r.selected) ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, 1, selItem);

        m_table->setItem(row, 2, new QTableWidgetItem(r.profile.name));
        m_table->setItem(row, 3, new QTableWidgetItem(r.profile.hostName));
        m_table->setItem(row, 4, new QTableWidgetItem(r.profile.user));
        m_table->setItem(row, 5, new QTableWidgetItem(QString::number(r.profile.port)));
        m_table->setItem(row, 6, new QTableWidgetItem(r.profile.identityFile));
        m_table->setItem(row, 7, new QTableWidgetItem(r.reason));

        // Make all text cells read-only
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

    m_applyBtn->setEnabled((selCreates + selUpdates) > 0);
}

void SshConfigImportPlanDialog::onFilterChanged()
{
    applyFiltersToRows();
}

void SshConfigImportPlanDialog::applyFiltersToRows()
{
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
    const auto creates = SshConfigImportPlan::selectedCreates(m_rows);
    const auto updates = SshConfigImportPlan::selectedUpdates(m_rows);

    if (creates.isEmpty() && updates.isEmpty()) {
        QMessageBox::information(this, "Import Plan", "Nothing selected to apply.");
        return;
    }

    const auto ans = QMessageBox::question(
        this,
        "Apply import",
        QString("Apply this plan?\n\nCreate: %1\nUpdate: %2\n\nThis will modify PQ-SSH profiles (not ~/.ssh/config).")
            .arg(creates.size()).arg(updates.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (ans != QMessageBox::Yes) return;

    emit applyRequested(creates, updates);
    close();
}
