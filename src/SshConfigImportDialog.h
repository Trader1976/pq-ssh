#pragma once
//
// ARCHITECTURE NOTES (SshConfigImportDialog.h)
//
// SshConfigImportDialog is a *read-only preview dialog* for an OpenSSH
// client configuration file (~/.ssh/config).
//
// Purpose:
// - Give the user visibility into what PQ-SSH detects in an OpenSSH config
//   before any import or profile creation occurs.
// - Display Host blocks, GLOBAL defaults, and parser warnings in a safe,
//   non-destructive way.
//
// Design boundaries:
// - This dialog does NOT create or modify PQ-SSH profiles.
// - It does NOT write to disk.
// - It does NOT apply import logic (that is handled by
//   SshConfigImportPlanDialog + SshConfigImportPlan).
//
// Data flow:
//   m_path (config file path)
//        ↓
//   SshConfigParser::parseFile()
//        ↓
//   m_result (SshConfigParseResult)
//        ↓
//   populate() → table + summary + warnings
//
// Lifetime:
// - Modeless QDialog (non-blocking).
// - reload() may be called multiple times to re-parse the file.
//
// Threading:
// - Parsing is currently synchronous and runs on the UI thread.
//   This is acceptable for v1 (single local file).
//

#include <QDialog>
#include "SshConfigParser.h"

// Forward declarations (Qt widgets)
class QLabel;
class QTableWidget;
class QPushButton;

class SshConfigImportDialog : public QDialog
{
    Q_OBJECT
public:
    // Construct dialog for a specific OpenSSH config file path.
    // The file is parsed immediately on construction.
    explicit SshConfigImportDialog(const QString& configPath,
                                   QWidget* parent = nullptr);

public slots:
    // Re-parse the config file and refresh the UI.
    // Safe to call repeatedly (e.g. after user edits ~/.ssh/config).
    void reload();

private:
    // UI construction (widgets + layout).
    void buildUi();

    // Populate labels and table from m_result.
    void populate();

private:
    // Absolute or user-provided path to ~/.ssh/config
    QString m_path;

    // Result of the most recent parse (blocks, includes, warnings).
    SshConfigParseResult m_result;

    // -------------------------
    // UI widgets
    // -------------------------
    QLabel* m_title    = nullptr;  // shows source file path
    QLabel* m_summary  = nullptr;  // counts + preview notice
    QLabel* m_warnings = nullptr;  // parser warnings (if any)

    QTableWidget* m_table = nullptr; // Host blocks preview table

    QPushButton* m_reloadBtn = nullptr;
    QPushButton* m_closeBtn  = nullptr;
};