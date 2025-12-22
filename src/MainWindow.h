#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QString>
#include <QByteArray>
#include <QUuid>
#include <QAction>

#include "SshProfile.h"
#include "SshClient.h"
#include "SshConfigImportPlan.h"
#include "SshConfigParser.h"



// Forward declarations (Qt / app)
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QLineEdit;
class QLabel;
class QCheckBox;
class QTabWidget;

class CpunkTermWidget;
class FilesTab;
class ProfilesEditorDialog;
class KeyGeneratorDialog;
class SettingsDialog;
class IdentityManagerDialog;
class SshConfigImportDialog;
class SshConfigImportPlanDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onSendInput();

    void onProfileDoubleClicked();
    void onProfileSelectionChanged(int row);
    void onEditProfilesClicked();

    void onFileDropped(const QString &path, const QByteArray &data);

    void downloadSelectionTriggered();
    void onOpenLogFile();
    void onOpenUserManual();
    void onTestUnlockDilithiumKey();
    void onInstallPublicKeyRequested(const QString& pubKeyLine, int profileIndex);
    void onOpenSettingsDialog();

    // ✅ NEW: receives negotiated KEX info from SshClient
    void onKexNegotiated(const QString& prettyText, const QString& rawKex);
    void onIdentityManagerRequested();
    void onImportOpenSshConfig();

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void setupUi();
    void setupMenus();

    void loadProfiles();
    void saveProfilesToDisk();

    bool probePqSupport(const QString &target);

    void appendTerminalLine(const QString &line);
    void updatePqStatusLabel(const QString &text, const QString &colorHex);

    QString m_sessionId;
    void logSessionInfo(const QString& msg);
    void uiInfo(const QString& msg);
    void uiWarn(const QString& msg);
    void uiDebug(const QString& msg);
    bool uiVerbose() const;

    void applySavedSettings();
    void onOpenSettings();
    void applyCurrentTheme();

    // Profile list grouping (Group headers + sorting)
    void rebuildProfileList();
    int  currentProfileIndex() const;
    void ensureProfileItemSelected();
    void onApplyImportedProfiles(const QVector<ImportedProfile>& creates,
                                 const QVector<ImportedProfile>& updates);

    // UI
    QListWidget    *m_profileList    = nullptr;
    QPushButton    *m_connectBtn     = nullptr;
    QPushButton    *m_disconnectBtn  = nullptr;
    QLineEdit      *m_hostField      = nullptr;
    QLabel         *m_statusLabel    = nullptr;

    QPlainTextEdit *m_terminal       = nullptr;
    QLineEdit      *m_inputField     = nullptr;
    QPushButton    *m_sendBtn        = nullptr;

    QLabel         *m_pqStatusLabel  = nullptr;
    QCheckBox      *m_pqDebugCheck   = nullptr;
    QCheckBox      *m_openInNewWindowCheck = nullptr;

    QPushButton    *m_editProfilesBtn = nullptr;

    // Modeless windows
    ProfilesEditorDialog *m_profilesEditor = nullptr;
    KeyGeneratorDialog   *m_keyGenerator   = nullptr;
    SettingsDialog       *m_settingsDlg    = nullptr;

    // State
    QVector<SshProfile> m_profiles;
    bool                m_pqActive = false;

    // Modules
    SshClient m_ssh;

    QTabWidget *m_mainTabs = nullptr;
    FilesTab   *m_filesTab = nullptr;

    // Terminal UI (QTermWidget-based)
    void openShellForProfile(const SshProfile &p, const QString &target, bool newWindow);
    CpunkTermWidget* createTerm(const SshProfile &p, QWidget *parent);
    void applyProfileToTerm(CpunkTermWidget *term, const SshProfile &p);

    // Hotkey macro wiring
    void installHotkeyMacro(CpunkTermWidget* term, QWidget* shortcutScope, const SshProfile& p);

    QMainWindow *m_tabbedShellWindow = nullptr;
    QTabWidget  *m_tabWidget         = nullptr;
    QAction     *m_devTestUnlockAct  = nullptr;
    IdentityManagerDialog *m_identityDlg = nullptr;
    SshConfigImportDialog *m_sshConfigDlg = nullptr;
    SshConfigImportPlanDialog* m_sshPlanDlg = nullptr;
    bool showStartupUnlockDialog();
    bool verifyAppPassword(const QString& pass) const;
    QLabel *m_versionLabel = nullptr;
};

#endif // MAINWINDOW_H
