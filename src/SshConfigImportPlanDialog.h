#pragma once
//
// SshConfigImportPlanDialog.h
//
// PURPOSE
// -------
// SshConfigImportPlanDialog provides a “review + selection” UI for importing entries
// from an OpenSSH config file (~/.ssh/config) into PQ-SSH profiles.
//
// It shows a generated import plan (Create / Update / Skip / Invalid) and allows the
// user to select which actionable rows (Create/Update) should be applied.
//
// ARCHITECTURAL ROLE / BOUNDARIES
// -------------------------------
// This dialog is intentionally an orchestration + presentation layer:
// - It does NOT parse ~/.ssh/config (caller provides SshConfigParseResult).
// - It does NOT persist PQ-SSH profiles directly.
// - It emits applyRequested(creates, updates) so the owning controller/service can apply.
// - It never modifies ~/.ssh/config; it only reads/parses and imports into PQ-SSH profiles.
//
// DATA FLOW
// ---------
//   Caller parses ssh config -> SshConfigParseResult
//                |
//                v
//   SshConfigImportPlan::buildPlan(parsed, existingNames, options) -> QVector<ImportPlanRow>
//                |
//                v
//   Dialog displays plan + selection
//                |
//                v
//   applyRequested(creates, updates) signal -> caller persists to PQ-SSH ProfileStore
//
// MODEL/VIEW NOTES
// ----------------
// - m_rows is the authoritative model for the plan + selection state.
// - QTableWidget is the view. User checkbox toggles are reflected back into m_rows.
//
// UX / SAFETY
// -----------
// - "Apply" is disabled unless at least one Create/Update row is selected.
// - "Allow updates" is opt-in because updating existing profiles is higher risk.
// - Filtering hides rows in the view only; it does not change the plan model.
//
// FUTURE EXTENSIONS
// -----------------
// - Replace QTableWidget with a proper QAbstractTableModel for scalability.
// - Add export of plan to JSON/CSV (“Save plan”) separate from apply.
// - Preserve selection across rebuilds when possible (stable key by profile name).

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
    // Constructs the dialog around an already-parsed ssh config result.
    //
    // Parameters:
    // - sourcePath: displayed to the user (source file path); not modified
    // - parsed: parsed SSH config (input data)
    // - existingProfileNames: used to decide Create vs Update vs Skip (collision logic)
    explicit SshConfigImportPlanDialog(const QString& sourcePath,
                                       const SshConfigParseResult& parsed,
                                       const QStringList& existingProfileNames,
                                       QWidget* parent = nullptr);

signals:
    // Emitted when the user confirms applying the selected plan.
    //
    // The dialog itself does not write changes; the caller should:
    // - create new PQ-SSH profiles for "creates"
    // - update existing PQ-SSH profiles for "updates"
    //
    // NOTE: These are profile changes only; ~/.ssh/config must remain untouched.
    void applyRequested(const QVector<ImportedProfile>& creates,
                        const QVector<ImportedProfile>& updates);

public slots:
    // Regenerates the plan model (m_rows) from current options and refreshes the view.
    // This is triggered by option changes and by the Refresh button.
    void rebuildPlan();

private slots:
    // Applies current filter controls to hide/unhide rows in the table.
    void onFilterChanged();

    // Convenience action to select/deselect all Create rows.
    // (Updates are intentionally not bulk-selected.)
    void onToggleSelectAllCreates(bool on);

    // Collects selected Create/Update rows, asks for confirmation, emits applyRequested.
    void onApply();

private:
    // Builds the UI widgets and connects signals/slots.
    void buildUi();

    // Re-populates QTableWidget from the plan model (m_rows).
    void populateTable();

    // Updates the summary label and Apply enabled state based on current model selection.
    void updateSummary();

    // Applies filter criteria (text + status) by hiding rows in the table view.
    void applyFiltersToRows();

    // Returns text used in the "Action" column and status filter matching.
    QString actionText(ImportAction a) const;

    // === Immutable-ish inputs (owned by dialog copy) ===
    QString m_path;                // for display ("Source: ..."), not modified
    SshConfigParseResult m_parsed; // parsed ssh config input
    QStringList m_existingNames;   // used for conflict detection / update decisions

    // === Plan generation options and model ===
    ImportPlanOptions m_opt;       // reflects UI option checkboxes
    QVector<ImportPlanRow> m_rows; // authoritative model (action + profile + reason + selected)

    // === UI widgets ===
    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;

    // Plan-generation options (cause rebuildPlan)
    QCheckBox* m_applyGlobal = nullptr;
    QCheckBox* m_skipWildcards = nullptr;
    QCheckBox* m_allowUpdates = nullptr;
    QCheckBox* m_normPaths = nullptr;

    // View-only filters (do not rebuild plan)
    QLineEdit* m_filterEdit = nullptr;
    QComboBox* m_statusFilter = nullptr;

    // Plan view
    QTableWidget* m_table = nullptr;

    // Actions
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_selectCreatesBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};