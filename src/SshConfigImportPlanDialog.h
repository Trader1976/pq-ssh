#pragma once
#include <QDialog>
#include <QStringList>

#include "SshConfigImportPlan.h"
#include "SshConfigParser.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QTableWidget;
class QPushButton;
class QCheckBox;

class SshConfigImportPlanDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SshConfigImportPlanDialog(const QString& sourcePath,
                                       const SshConfigParseResult& parsed,
                                       const QStringList& existingProfileNames,
                                       QWidget* parent = nullptr);

    signals:
        void applyRequested(const QVector<ImportedProfile>& creates,
                            const QVector<ImportedProfile>& updates);

public slots:
    void rebuildPlan();

private slots:
    void onFilterChanged();
    void onToggleSelectAllCreates(bool on);
    void onApply();

private:
    void buildUi();
    void populateTable();
    void updateSummary();
    void applyFiltersToRows();

    QString actionText(ImportAction a) const;

    QString m_path;
    SshConfigParseResult m_parsed;
    QStringList m_existingNames;

    ImportPlanOptions m_opt;
    QVector<ImportPlanRow> m_rows;

    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;

    QCheckBox* m_applyGlobal = nullptr;
    QCheckBox* m_skipWildcards = nullptr;
    QCheckBox* m_allowUpdates = nullptr;
    QCheckBox* m_normPaths = nullptr;

    QLineEdit* m_filterEdit = nullptr;
    QComboBox* m_statusFilter = nullptr;

    QTableWidget* m_table = nullptr;

    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_selectCreatesBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};
