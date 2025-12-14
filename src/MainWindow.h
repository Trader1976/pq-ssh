#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QString>
#include <QByteArray>

#include "SshProfile.h"
#include "SshClient.h"
#include <QUuid>


// Forward declarations (Qt)
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QLineEdit;
class QLabel;
class QCheckBox;

class QTabWidget;
class CpunkTermWidget;

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

    // (Will be wired once ShellManager exists)
    void onFileDropped(const QString &path, const QByteArray &data);

    void downloadSelectionTriggered();
    void onOpenLogFile();

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

    // State
    QVector<SshProfile> m_profiles;
    bool                m_pqActive = false; // optional; currently not used in cleaned cpp

    // Modules
    SshClient m_ssh;


    // Terminal UI (QTermWidget-based)
    void openShellForProfile(const SshProfile &p, const QString &target, bool newWindow);
    CpunkTermWidget* createTerm(const SshProfile &p, QWidget *parent);
    void applyProfileToTerm(CpunkTermWidget *term, const SshProfile &p);

    QMainWindow *m_tabbedShellWindow = nullptr;
    QTabWidget  *m_tabWidget = nullptr;

};

#endif // MAINWINDOW_H
