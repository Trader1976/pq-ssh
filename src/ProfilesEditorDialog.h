#pragma once

#include <QDialog>
#include <QVector>

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

class ProfilesEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfilesEditorDialog(const QVector<SshProfile> &profiles,
                                  int initialRow = 0,
                                  QWidget *parent = nullptr);

    QVector<SshProfile> resultProfiles() const { return m_result; }

private:
    void buildUi();

    // Profiles
    void loadProfileToForm(int row);
    void syncFormToCurrent();
    void addProfile();
    void deleteProfile();
    bool validateProfiles(QString *errMsg) const;
    bool isMacroEmpty(const ProfileMacro& m) const;
    QString macroDisplayName(const ProfileMacro& m, int idx) const;

    // Macros (multi)
    void loadMacroToEditor(int macroRow);
    void syncMacroEditorToCurrent();
    int  currentMacroIndex() const;
    void rebuildMacroList();                 // refresh list UI from profile.macros
    void ensureMacroSelectionValid();        // keep selection sane after add/delete
    void addMacro();
    void deleteMacro();
    void clearMacroShortcut();
    void loadMacroToForm(int row);
    void syncMacroToCurrent();          // (macro editor -> current macro)


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

private:
    QVector<SshProfile> m_working;
    QVector<SshProfile> m_result;

    int m_currentRow = -1;       // selected profile row (m_working)
    int m_currentMacroRow = -1;  // selected macro row (within m_working[m_currentRow].macros)

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

    // Right: macros column (multi)
    QListWidget *m_macroList = nullptr;
    QPushButton *m_macroAddBtn = nullptr;
    QPushButton *m_macroDelBtn = nullptr;

    // Macro editor (edits selected macro)
    QLineEdit        *m_macroNameEdit = nullptr;       // ProfileMacro::name
    QKeySequenceEdit *m_macroShortcutEdit = nullptr;   // ProfileMacro::shortcut
    QPushButton      *m_macroClearBtn = nullptr;
    QLineEdit        *m_macroCmdEdit = nullptr;        // ProfileMacro::command
    QCheckBox        *m_macroEnterCheck = nullptr;     // ProfileMacro::sendEnter

    // Save / Cancel
    QDialogButtonBox *m_buttonsBox = nullptr;
    // import / export macros
    QJsonObject macrosToJson(const QVector<ProfileMacro>& macros) const;
    QVector<ProfileMacro> macrosFromJson(const QJsonObject& obj, QString* err) const;
    QPushButton* m_macroImportBtn = nullptr;
    QPushButton* m_macroExportBtn = nullptr;
};
