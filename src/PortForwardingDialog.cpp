#include "PortForwardingDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMessageBox>
#include <QInputDialog>

static QString typeLabel(PortForwardType t)
{
    if (t == PortForwardType::Local) return "Local (-L)";
    if (t == PortForwardType::Remote) return "Remote (-R)";
    return "Dynamic (-D)";
}

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

static bool editRule(QWidget *parent, PortForwardRule *r)
{
    if (!r) return false;

    QStringList types = {"local", "remote", "dynamic"};
    bool ok = false;
    const QString t = QInputDialog::getItem(parent, QObject::tr("Forward type"),
                                           QObject::tr("Type:"), types,
                                           types.indexOf(portForwardTypeToString(r->type)), false, &ok);
    if (!ok) return false;
    r->type = portForwardTypeFromString(t);

    const QString bind = QInputDialog::getText(parent, QObject::tr("Bind address"),
                                               QObject::tr("Bind address:"), QLineEdit::Normal,
                                               r->bind, &ok);
    if (!ok) return false;
    r->bind = bind.trimmed();

    const int lp = QInputDialog::getInt(parent, QObject::tr("Listen port"),
                                        QObject::tr("Listen port:"), r->listenPort, 1, 65535, 1, &ok);
    if (!ok) return false;
    r->listenPort = lp;

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

    const QString note = QInputDialog::getText(parent, QObject::tr("Description"),
                                               QObject::tr("Description (optional):"),
                                               QLineEdit::Normal, r->note, &ok);
    if (!ok) return false;
    r->note = note.trimmed();

    return true;
}

PortForwardingDialog::PortForwardingDialog(const QVector<PortForwardRule> &rules, QWidget *parent)
    : QDialog(parent), m_rules(rules)
{
    setWindowTitle(tr("Port forwarding"));
    resize(760, 420);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10,10,10,10);
    outer->setSpacing(8);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Enabled"), tr("Type"), tr("Rule"), tr("Note")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);

    auto *btnRow = new QWidget(this);
    auto *btnL = new QHBoxLayout(btnRow);
    btnL->setContentsMargins(0,0,0,0);
    btnL->setSpacing(6);

    m_addBtn = new QPushButton(tr("Add"), btnRow);
    m_editBtn = new QPushButton(tr("Edit"), btnRow);
    m_delBtn = new QPushButton(tr("Remove"), btnRow);

    btnL->addWidget(m_addBtn);
    btnL->addWidget(m_editBtn);
    btnL->addWidget(m_delBtn);
    btnL->addStretch(1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    outer->addWidget(m_table, 1);
    outer->addWidget(btnRow, 0);
    outer->addWidget(box, 0);

    connect(m_addBtn, &QPushButton::clicked, this, &PortForwardingDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &PortForwardingDialog::onEdit);
    connect(m_delBtn, &QPushButton::clicked, this, &PortForwardingDialog::onRemove);

    connect(m_table, &QTableWidget::cellChanged, this, &PortForwardingDialog::onToggleEnabled);

    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuild();
}

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

void PortForwardingDialog::onToggleEnabled(int row, int col)
{
    if (col != 0) return;
    if (row < 0 || row >= m_rules.size()) return;
    auto *it = m_table->item(row, 0);
    if (!it) return;
    m_rules[row].enabled = (it->checkState() == Qt::Checked);
}

void PortForwardingDialog::onAdd()
{
    PortForwardRule r;
    r.enabled = true;
    r.bind = "127.0.0.1";
    r.targetHost = "localhost";

    if (!editRule(this, &r)) return;
    m_rules.push_back(r);
    rebuild();
}

void PortForwardingDialog::onEdit()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rules.size()) return;

    PortForwardRule r = m_rules[row];
    if (!editRule(this, &r)) return;

    m_rules[row] = r;
    rebuild();
}

void PortForwardingDialog::onRemove()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rules.size()) return;

    m_rules.remove(row);
    rebuild();
}
