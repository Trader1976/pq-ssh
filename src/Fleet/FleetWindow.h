#pragma once

#include <QMainWindow>
#include <QVector>
#include <QPointer>

#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QCheckBox>

#include "FleetExecutor.h"
#include "../ProfileStore.h" // SshProfile

class FleetWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit FleetWindow(const QVector<SshProfile>& profiles, QWidget* parent = nullptr);
    ~FleetWindow() override;

private slots:
    void onFilterChanged(const QString& text);
    void onSelectAll();
    void onSelectNone();
    void onRunClicked();
    void onCancelClicked();
    void onActionChanged(int idx);
    void onResultRowActivated(int row, int col);

    void onJobStarted(const FleetJob& job);
    void onJobProgress(const FleetJob& job, int done, int total);
    void onJobFinished(const FleetJob& job);

private:
    void buildUi();
    void rebuildTargetsList();
    QVector<int> selectedProfileIndexes() const;

    void clearResults();
    void appendLog(const QString& line);
    void upsertResultRow(const FleetTargetResult& r);
    QString stateToText(FleetTargetState st) const;

private:
    QVector<SshProfile> m_profiles;

    // UI
    QLineEdit*   m_filterEdit = nullptr;
    QListWidget* m_targetsList = nullptr;
    QPushButton* m_selectAllBtn = nullptr;
    QPushButton* m_selectNoneBtn = nullptr;

    QComboBox*   m_actionCombo = nullptr;
    QStackedWidget* m_actionStack = nullptr;

    // Action pages
    QLineEdit* m_cmdEdit = nullptr;          // RunCommand
    QLineEdit* m_serviceEdit = nullptr;      // Check/Restart service
    QCheckBox* m_confirmDanger = nullptr;    // for restart

    QSpinBox*    m_concurrencySpin = nullptr;

    QPushButton* m_runBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    QLabel*       m_statusLabel = nullptr;
    QTableWidget* m_resultsTable = nullptr;
    QPlainTextEdit* m_log = nullptr;

    // Engine
    FleetExecutor* m_exec = nullptr;

    // For table row mapping
    // key: profileIndex -> row
    QHash<int,int> m_rowByProfile;
    QSpinBox* m_timeoutSpin = nullptr;   // command timeout (seconds)
};
