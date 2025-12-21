#pragma once
#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QToolButton;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

private:
    void buildUi();
    void loadFromSettings();
    void saveToSettings();

    QString dirOfPathOrEmpty(const QString& path) const;

private slots:
    void onAccepted();

    void onBrowseLogFile();
    void onOpenLogDir();

    void onBrowseAuditDir();
    void onOpenAuditDir();

private:
    QComboBox *m_themeCombo = nullptr;
    QComboBox *m_logLevelCombo = nullptr;

    // NEW: paths
    QLineEdit  *m_logFileEdit = nullptr;
    QToolButton *m_logBrowseBtn = nullptr;
    QToolButton *m_logOpenDirBtn = nullptr;

    QLineEdit  *m_auditDirEdit = nullptr;
    QToolButton *m_auditBrowseBtn = nullptr;
    QToolButton *m_auditOpenDirBtn = nullptr;

    QDialogButtonBox *m_buttons = nullptr;
};
