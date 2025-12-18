#pragma once

#include <QDialog>
#include <QVector>

#include "SshProfile.h"

class QListWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QKeySequenceEdit;

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
    void loadProfileToForm(int row);
    void syncFormToCurrent();
    void addProfile();
    void deleteProfile();
    bool validateProfiles(QString *errMsg) const;

private slots:
    void onListRowChanged(int row);
    void onNameEdited(const QString &text);
    void onAccepted();

private:
    QVector<SshProfile> m_working;
    QVector<SshProfile> m_result;
    int m_currentRow = -1;

    // -------------------------
    // UI widgets
    // -------------------------
    QListWidget *m_list = nullptr;

    // Core connection settings
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox  *m_portSpin = nullptr;

    // Group selector
    QComboBox *m_groupCombo = nullptr;

    // Debug toggle
    QCheckBox *m_pqDebugCheck = nullptr;

    // Terminal appearance / geometry settings
    QComboBox *m_colorSchemeCombo = nullptr;
    QSpinBox  *m_fontSizeSpin = nullptr;
    QSpinBox  *m_widthSpin = nullptr;
    QSpinBox  *m_heightSpin = nullptr;

    // Scrollback (0 = unlimited)
    QSpinBox *m_historySpin = nullptr;

    // Key-based auth (existing)
    QComboBox *m_keyTypeCombo = nullptr;
    QLineEdit *m_keyFileEdit  = nullptr;

    // “3rd column” / post-connect actions
    QComboBox *m_assignedKeyCombo = nullptr;          // SshProfile::assignedKey
    QLineEdit *m_postConnectCmdEdit = nullptr;        // SshProfile::postConnectCommand
    QCheckBox *m_postConnectCmdEnterCheck = nullptr;  // SshProfile::postConnectEnter

    // Hotkey macro (single)
    QKeySequenceEdit *m_macroShortcutEdit = nullptr;  // SshProfile::macroShortcut
    QLineEdit *m_macroCmdEdit = nullptr;              // SshProfile::macroCommand
    QCheckBox *m_macroEnterCheck = nullptr;           // SshProfile::macroEnter

    // Save / Cancel
    QDialogButtonBox *m_buttonsBox = nullptr;
};
