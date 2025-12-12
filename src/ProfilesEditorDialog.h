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

class ProfilesEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfilesEditorDialog(const QVector<SshProfile> &profiles, QWidget *parent = nullptr);

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

    // UI
    QListWidget *m_list = nullptr;

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox  *m_portSpin = nullptr;

    QCheckBox *m_pqDebugCheck = nullptr;

    QComboBox *m_colorSchemeCombo = nullptr;
    QSpinBox  *m_fontSizeSpin = nullptr;
    QSpinBox  *m_widthSpin = nullptr;
    QSpinBox  *m_heightSpin = nullptr;

    QDialogButtonBox *m_buttonsBox = nullptr;
};
