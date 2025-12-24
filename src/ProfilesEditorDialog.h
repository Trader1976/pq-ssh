#pragma once

// ProfilesEditorDialog.h
//
// ARCHITECTURE NOTES (ProfilesEditorDialog.h)
//
// This dialog is the *working-copy editor* for SSH profiles.
// It edits a local copy (m_working) and only commits to m_result on Save (Accepted).
//
// Responsibilities:
// - UI construction (3-column splitter: profiles list / profile details / macros).
// - Validation (user+host must exist; optional sanity for key fields).
// - Macro list editing (add/delete/select + editor + import/export).
//
// Non-responsibilities:
// - No disk I/O (ProfileStore owns persistence).
// - No SSH/network operations (MainWindow/SshClient own connectivity).
//
// Data flow:
//   caller provides profiles -> m_working (editable copy)
//   user edits -> syncFormToCurrent()/syncMacroEditorToCurrent()
//   Save -> validateProfiles() -> m_result = m_working
//

#include <QDialog>
#include <QVector>
#include <QString>

#include "SshProfile.h"

// Forward declarations (Qt)
class QListWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QPushButton;
class QKeySequenceEdit;
class QLabel;
class QJsonObject;
class QPlainTextEdit;

class ProfilesEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfilesEditorDialog(const QVector<SshProfile> &profiles,
                                  int initialRow = 0,
                                  QWidget *parent = nullptr);

    QVector<SshProfile> resultProfiles() const { return m_result; }

private:
    // UI assembly (no persistence / no SSH)
    void buildUi();

    // -------------------------
    // Profiles (m_working)
    // -------------------------
    void loadProfileToForm(int row);
    void syncFormToCurrent();
    void addProfile();
    void deleteProfile();
    bool validateProfiles(QString *errMsg) const;

    // -------------------------
    // Macros (multi)
    // -------------------------
    bool isMacroEmpty(const ProfileMacro& m) const;
    QString macroDisplayName(const ProfileMacro& m, int idx) const;

    void loadMacroToEditor(int macroRow);
    void syncMacroEditorToCurrent();

    int  currentMacroIndex() const;
    void rebuildMacroList();                 // refresh list UI from profile.macros
    void ensureMacroSelectionValid();        // keep selection sane after add/delete

    void addMacro();
    void deleteMacro();
    void clearMacroShortcut();

    // Import/export helpers (UI convenience; does not touch ProfileStore)
    QJsonObject macrosToJson(const QVector<ProfileMacro>& macros) const;
    QVector<ProfileMacro> macrosFromJson(const QJsonObject& obj, QString* err) const;

private slots:
    // Profiles
    void onListRowChanged(int row);
    void onNameEdited(const QString &text);
    void onAccepted();

    // Macros
    void onMacroRowChanged(int row);
    void onMacroNameEdited(const QString &text);
    void importMacros();
    void exportMacros();
    void onClearKeyFile();
    void onProbeCrypto();
    void updateProbeButtonEnabled();


private:
    QVector<SshProfile> m_working;
    QVector<SshProfile> m_result;

    int m_currentRow = -1;       // selected profile row (m_working)
    int m_currentMacroRow = -1;  // selected macro row within m_working[m_currentRow].macros

    // -------------------------
    // UI widgets
    // -------------------------

    // Left: profiles list
    QListWidget *m_list = nullptr;

    // Middle: core profile settings
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox  *m_portSpin = nullptr;

    QComboBox *m_groupCombo = nullptr;
    QCheckBox *m_pqDebugCheck = nullptr;

    QComboBox *m_colorSchemeCombo = nullptr;
    QSpinBox  *m_fontSizeSpin = nullptr;
    QSpinBox  *m_widthSpin = nullptr;
    QSpinBox  *m_heightSpin = nullptr;
    QSpinBox  *m_historySpin = nullptr;

    QComboBox *m_keyTypeCombo = nullptr;
    QLineEdit *m_keyFileEdit  = nullptr;
    QPushButton *m_keyClearBtn = nullptr;

    // Right: macros column (multi)
    QListWidget *m_macroList = nullptr;
    QPushButton *m_macroAddBtn = nullptr;
    QPushButton *m_macroDelBtn = nullptr;
    QPushButton *m_probeBtn = nullptr;

    // Macro editor (edits selected macro)
    QLineEdit        *m_macroNameEdit = nullptr;       // ProfileMacro::name
    QKeySequenceEdit *m_macroShortcutEdit = nullptr;   // ProfileMacro::shortcut
    QPushButton      *m_macroClearBtn = nullptr;
    QLineEdit        *m_macroCmdEdit = nullptr;        // ProfileMacro::command
    QCheckBox        *m_macroEnterCheck = nullptr;     // ProfileMacro::sendEnter

    // Import/export macros
    QPushButton *m_macroImportBtn = nullptr;
    QPushButton *m_macroExportBtn = nullptr;

    // Save / Cancel
    QDialogButtonBox *m_buttonsBox = nullptr;
};