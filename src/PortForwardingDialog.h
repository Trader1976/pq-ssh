#pragma once
#include <QDialog>
#include <QVector>
#include "SshProfile.h"

class QTableWidget;
class QPushButton;

class PortForwardingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PortForwardingDialog(const QVector<PortForwardRule> &rules, QWidget *parent=nullptr);
    QVector<PortForwardRule> rules() const { return m_rules; }

private slots:
    void onAdd();
    void onEdit();
    void onRemove();
    void onToggleEnabled(int row, int col);

private:
    void rebuild();

    QVector<PortForwardRule> m_rules;
    QTableWidget *m_table = nullptr;
    QPushButton *m_addBtn = nullptr;
    QPushButton *m_editBtn = nullptr;
    QPushButton *m_delBtn = nullptr;
};
