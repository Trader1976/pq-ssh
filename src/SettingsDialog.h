#pragma once
#include <QDialog>

class QComboBox;
class QDialogButtonBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

private:
    void buildUi();
    void loadFromSettings();
    void saveToSettings();

private slots:
    void onAccepted();

private:
    QComboBox *m_themeCombo = nullptr;
    QComboBox *m_logLevelCombo = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
