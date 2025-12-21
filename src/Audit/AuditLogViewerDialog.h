// src/Audit/AuditLogViewerDialog.h
#pragma once

#include <QDialog>

class QLineEdit;
class QTableView;
class QTextBrowser;
class QToolButton;
class QDateEdit;
class QSortFilterProxyModel;

#include "AuditLogModel.h"
class QTextBrowser;
class AuditLogViewerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AuditLogViewerDialog(QWidget* parent=nullptr);

private slots:
    void onReload();
    void onOpenAuditDir();
    void onRowChanged();
    void onSearchChanged(const QString&);

private:
    QString auditFileForDate(const QDate& d) const;
    QString auditBaseDir() const;

    void loadDate(const QDate& d);
    void showDetails(const AuditLogEntry& e);

private:
    AuditLogModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;

    QDateEdit* m_date = nullptr;
    QLineEdit* m_search = nullptr;
    QToolButton* m_reloadBtn = nullptr;
    QToolButton* m_openDirBtn = nullptr;

    QTableView* m_table = nullptr;
    QTextBrowser* m_details = nullptr;
};
