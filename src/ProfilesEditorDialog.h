#pragma once

#include <QDialog>
#include <QVector>

#include "SshProfile.h" // Profile data model (host/user/port/theme/auth settings)

class QListWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;

/**
 * ProfilesEditorDialog
 *
 * Purpose:
 *  - GUI editor for SSH connection profiles (stored via ProfileStore as JSON).
 *  - Lets users add/edit/remove profiles without touching profiles.json manually.
 *
 * Data model:
 *  - Takes an initial list of SshProfile objects ("working copy")
 *  - User edits the working copy via form controls
 *  - On Save, validates and returns the updated list via resultProfiles()
 *
 * UI layout (high-level):
 *  - Left: list of profiles (names)
 *  - Right: editable form for the currently selected profile
 *
 * Notes:
 *  - This dialog does not do networking. It only edits configuration.
 *  - The caller (MainWindow) decides what to do with the returned profiles.
 */
class ProfilesEditorDialog : public QDialog
{
    Q_OBJECT
public:
    /**
     * @param profiles   Current profiles (copied into m_working).
     * @param initialRow Which row to select initially (matches MainWindow selection).
     */
    explicit ProfilesEditorDialog(const QVector<SshProfile> &profiles,
                                  int initialRow = 0,
                                  QWidget *parent = nullptr);

    /**
     * Returns the final edited profiles (valid only after dialog Accepted).
     * If dialog was cancelled, caller should ignore this and keep original list.
     */
    QVector<SshProfile> resultProfiles() const { return m_result; }

private:
    // Builds widgets, layout, and signal wiring.
    void buildUi();

    // Loads a profile's values into the form controls.
    // row == -1 means "no selection" (clears form to defaults).
    void loadProfileToForm(int row);

    // Writes current form values back into the working profile (m_working[m_currentRow]).
    // Called when changing selection and before saving.
    void syncFormToCurrent();

    // Adds a new profile with sane defaults and selects it.
    void addProfile();

    // Deletes the currently selected profile.
    void deleteProfile();

    // Ensures required fields are present and cross-field rules make sense.
    // Example rule: if key_type != auto, then key_file must not be empty.
    bool validateProfiles(QString *errMsg) const;

private slots:
    /**
     * Selection change handler:
     *  - store current form -> current profile (syncFormToCurrent)
     *  - update m_currentRow
     *  - load new profile -> form (loadProfileToForm)
     */
    void onListRowChanged(int row);

    /**
     * Keeps list item text in sync while editing the Name field.
     * If name is blank, uses "user@host" as a live fallback display.
     */
    void onNameEdited(const QString &text);

    /**
     * Save handler:
     *  - sync current form
     *  - validate
     *  - copy m_working -> m_result
     *  - accept()
     */
    void onAccepted();

private:
    // Working copy edited by the dialog (safe to mutate).
    QVector<SshProfile> m_working;

    // Final result returned to caller after Save/Accept.
    QVector<SshProfile> m_result;

    // Currently selected row index into m_working / m_list.
    int m_currentRow = -1;

    // -------------------------
    // UI widgets
    // -------------------------

    // Left side list (profile names)
    QListWidget *m_list = nullptr;

    // Core connection settings
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox  *m_portSpin = nullptr;

    // NEW: group selector (editable dropdown; empty => treated as "Ungrouped")
    QComboBox *m_groupCombo = nullptr;

    // Debug toggle (e.g. verbose logging / -vv style behavior)
    QCheckBox *m_pqDebugCheck = nullptr;

    // Terminal appearance / geometry settings
    QComboBox *m_colorSchemeCombo = nullptr;
    QSpinBox  *m_fontSizeSpin = nullptr;
    QSpinBox  *m_widthSpin = nullptr;
    QSpinBox  *m_heightSpin = nullptr;

    // NEW: terminal scrollback (0 = unlimited)
    QSpinBox *m_historySpin = nullptr;

    // Key-based auth settings (future expansion)
    // keyType examples: "auto", "openssh", "pq" (placeholder)
    QComboBox *m_keyTypeCombo = nullptr;
    QLineEdit *m_keyFileEdit  = nullptr;

    // Save / Cancel buttons
    QDialogButtonBox *m_buttonsBox = nullptr;
};
