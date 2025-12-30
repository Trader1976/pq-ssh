#include "PortForwardingDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMessageBox>
#include <QInputDialog>

// PortForwardingDialog.cpp
// ------------------------
// Simple editor dialog for profile port-forwarding rules.
//
// Responsibilities:
// - Present the current list of PortForwardRule entries in a table
// - Allow add/edit/remove of rules via basic input dialogs
// - Allow toggling enabled state via checkbox in table
// - Validate enabled rules on OK (basic sanity + duplicate bind prevention)
//
// Non-responsibilities:
// - Does not start ssh or apply rules to a live session
// - Does not persist to disk (ProfileStore handles JSON persistence)
// - Does not implement advanced validation (e.g., IP format validation)

// Convert PortForwardType into a human-readable short label shown in the table.
static QString typeLabel(PortForwardType t)
{
    if (t == PortForwardType::Local)  return "Local (-L)";
    if (t == PortForwardType::Remote) return "Remote (-R)";
    return "Dynamic (-D)";
}

// Format a rule into a compact human-readable “spec” label shown in the table.
static QString ruleLabel(const PortForwardRule &r)
{
    const QString bind = r.bind.trimmed().isEmpty() ? "127.0.0.1" : r.bind.trimmed();

    if (r.type == PortForwardType::Dynamic)
        return QString("%1:%2").arg(bind).arg(r.listenPort);

    return QString("%1:%2 → %3:%4")
        .arg(bind).arg(r.listenPort)
        .arg(r.targetHost.trimmed().isEmpty() ? "localhost" : r.targetHost.trimmed())
        .arg(r.targetPort);
}

// Interactive editor for a single PortForwardRule using QInputDialog prompts.
// Returns true if the user completed the edit, false if cancelled at any step.
//
// Edits:
// - type (local/remote/dynamic)
// - bind address
// - listen port
// - target host/port (not for Dynamic)
// - optional note
static bool editRule(QWidget *parent, PortForwardRule *r)
{
    if (!r) return false;

    QStringList types = {"local", "remote", "dynamic"};

    // Type
    bool ok = false;
    const QString t = QInputDialog::getItem(parent, QObject::tr("Forward type"),
                                           QObject::tr("Type:"), types,
                                           types.indexOf(portForwardTypeToString(r->type)), false, &ok);
    if (!ok) return false;
    r->type = portForwardTypeFromString(t);

    // Bind address (empty -> treated as default later)
    const QString bind = QInputDialog::getText(parent, QObject::tr("Bind address"),
                                               QObject::tr("Bind address:"), QLineEdit::Normal,
                                               r->bind, &ok);
    if (!ok) return false;
    r->bind = bind.trimmed();

    // Listen port (required)
    const int lp = QInputDialog::getInt(parent, QObject::tr("Listen port"),
                                        QObject::tr("Listen port:"), r->listenPort, 1, 65535, 1, &ok);
    if (!ok) return false;
    r->listenPort = lp;

    // Target (only for Local/Remote)
    if (r->type != PortForwardType::Dynamic) {
        const QString th = QInputDialog::getText(parent, QObject::tr("Target host"),
                                                 QObject::tr("Target host:"), QLineEdit::Normal,
                                                 r->targetHost, &ok);
        if (!ok) return false;
        r->targetHost = th.trimmed();

        const int tp = QInputDialog::getInt(parent, QObject::tr("Target port"),
                                            QObject::tr("Target port:"), r->targetPort, 1, 65535, 1, &ok);
        if (!ok) return false;
        r->targetPort = tp;
    }

    // Optional note / description
    const QString note = QInputDialog::getText(parent, QObject::tr("Description"),
                                               QObject::tr("Description (optional):"),
                                               QLineEdit::Normal, r->note, &ok);
    if (!ok) return false;
    r->note = note.trimmed();

    return true;
}

// Create a port-forwarding editor dialog initialized with the provided rules.
// The edited rules can be retrieved from the dialog instance (via your header API).
PortForwardingDialog::PortForwardingDialog(const QVector<PortForwardRule> &rules, QWidget *parent)
    : QDialog(parent), m_rules(rules)
{
    setWindowTitle(tr("Port forwarding"));
    resize(760, 420);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10,10,10,10);
    outer->setSpacing(8);

    // Rules table: Enabled checkbox + Type + Rule spec + Note
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Enabled"), tr("Type"), tr("Rule"), tr("Note")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);

    // Button row (Add/Edit/Remove)
    auto *btnRow = new QWidget(this);
    auto *btnL = new QHBoxLayout(btnRow);
    btnL->setContentsMargins(0,0,0,0);
    btnL->setSpacing(6);

    m_addBtn  = new QPushButton(tr("Add"), btnRow);
    m_editBtn = new QPushButton(tr("Edit"), btnRow);
    m_delBtn  = new QPushButton(tr("Remove"), btnRow);

    btnL->addWidget(m_addBtn);
    btnL->addWidget(m_editBtn);
    btnL->addWidget(m_delBtn);
    btnL->addStretch(1);

    // OK / Cancel
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    outer->addWidget(m_table, 1);
    outer->addWidget(btnRow, 0);
    outer->addWidget(box, 0);

    // Wiring
    connect(m_addBtn,  &QPushButton::clicked, this, &PortForwardingDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &PortForwardingDialog::onEdit);
    connect(m_delBtn,  &QPushButton::clicked, this, &PortForwardingDialog::onRemove);

    // Column 0 checkbox changes map to enabled state
    connect(m_table, &QTableWidget::cellChanged, this, &PortForwardingDialog::onToggleEnabled);

    // OK validates then accepts
    connect(box, &QDialogButtonBox::accepted, this, &PortForwardingDialog::onAccept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuild();
}

// Rebuild the table UI from m_rules.
// Signals are blocked during population to avoid triggering onToggleEnabled().
void PortForwardingDialog::rebuild()
{
    m_table->blockSignals(true);
    m_table->setRowCount(m_rules.size());

    for (int i=0; i<m_rules.size(); ++i) {
        const auto &r = m_rules[i];

        auto *enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(enabledItem->flags() | Qt::ItemIsUserCheckable);
        enabledItem->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);

        m_table->setItem(i, 0, enabledItem);
        m_table->setItem(i, 1, new QTableWidgetItem(typeLabel(r.type)));
        m_table->setItem(i, 2, new QTableWidgetItem(ruleLabel(r)));
        m_table->setItem(i, 3, new QTableWidgetItem(r.note));
    }

    m_table->resizeColumnsToContents();
    m_table->blockSignals(false);
}

// Handle checkbox toggles in the Enabled column and update m_rules accordingly.
void PortForwardingDialog::onToggleEnabled(int row, int col)
{
    if (col != 0) return;
    if (row < 0 || row >= m_rules.size()) return;
    auto *it = m_table->item(row, 0);
    if (!it) return;
    m_rules[row].enabled = (it->checkState() == Qt::Checked);
}

// Add a new rule by creating a reasonable default template and running editRule().
// If the user cancels editing, nothing is added.
void PortForwardingDialog::onAdd()
{
    PortForwardRule r;
    r.enabled    = true;
    r.type       = PortForwardType::Local;

    r.bind       = "127.0.0.1";
    r.listenPort = 18080;        // nicer than 0 in the prompt

    r.targetHost = "localhost";
    r.targetPort = 8080;         // nicer than 0 in the prompt

    if (!editRule(this, &r)) return;
    m_rules.push_back(r);
    rebuild();
}

// Edit the currently selected rule.
// If the user cancels, the rule remains unchanged.
void PortForwardingDialog::onEdit()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rules.size()) return;

    PortForwardRule r = m_rules[row];
    if (!editRule(this, &r)) return;

    m_rules[row] = r;
    rebuild();
}

// Remove the currently selected rule.
void PortForwardingDialog::onRemove()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rules.size()) return;

    m_rules.remove(row);
    rebuild();
}

// Validate enabled rules and accept the dialog if all are sane.
// Validation performed:
// - listen port range
// - for Local/Remote: target host non-empty and target port range
// - duplicate detection per (type, bindHost, listenPort) among enabled rules
void PortForwardingDialog::onAccept()
{
    QSet<QString> seen;

    for (int i = 0; i < m_rules.size(); ++i) {
        const PortForwardRule &f = m_rules[i];
        if (!f.enabled) continue;

        auto fail = [&](const QString &msg) {
            QMessageBox::warning(
                this,
                tr("Invalid port forwarding rule"),
                tr("Rule #%1: %2").arg(i + 1).arg(msg)
            );
        };

        // listenPort required for all types
        if (f.listenPort < 1 || f.listenPort > 65535) {
            fail(tr("Invalid listen port (%1).").arg(f.listenPort));
            return;
        }

        const QString bindHost = f.bind.trimmed().isEmpty()
            ? QStringLiteral("127.0.0.1")
            : f.bind.trimmed();

        // Local/Remote require target host+port
        if (f.type != PortForwardType::Dynamic) {
            if (f.targetHost.trimmed().isEmpty()) {
                fail(tr("Target host is empty."));
                return;
            }
            if (f.targetPort < 1 || f.targetPort > 65535) {
                fail(tr("Invalid target port (%1).").arg(f.targetPort));
                return;
            }
        }

        const QString typeStr =
            (f.type == PortForwardType::Local)  ? "L" :
            (f.type == PortForwardType::Remote) ? "R" : "D";

        const QString key = QString("%1|%2|%3").arg(typeStr, bindHost).arg(f.listenPort);
        if (seen.contains(key)) {
            fail(tr("Duplicate bind %1:%2 (%3).")
                     .arg(bindHost)
                     .arg(f.listenPort)
                     .arg(typeStr));
            return;
        }
        seen.insert(key);
    }

    accept();
}
