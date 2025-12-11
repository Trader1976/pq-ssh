#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QProcess>
#include <QVector>
#include <QThread>
#include "SshShellWorker.h"
#include <QPlainTextEdit>
#include <QThread>
#include "TerminalView.h"
#include <libssh/libssh.h>

class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QLineEdit;
class QLabel;
class QCheckBox;
class QDialog;
class QPlainTextEdit;
class QTermWidget;

// Simple SSH profile representation
struct SshProfile {
    QString name;
    QString user;
    QString host;
    int     port    = 22;
    bool    pqDebug = true;

    // NEW: visual settings
    QString termColorScheme;  // e.g. "WhiteOnBlack"
    int     termFontSize = 11;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onConnectClicked();
    void onProfileDoubleClicked();
    void onProfileSelectionChanged(int row);
    void onSendInput();
    void onDisconnectClicked();
    void onEditProfilesClicked();
    void onUserTyped(const QByteArray &data); 

    void handleSshReadyRead();
    void handleSshFinished(int exitCode, QProcess::ExitStatus status);
    void handleSshError(QProcess::ProcessError error);

    void startInteractiveShell();          // call after login or from a button
    void handleShellOutput(const QByteArray &data);
    void handleShellClosed(const QString &reason);
    void startColorShell(const QString &target);

private:
    void setupUi();
    void setupMenus();

    // Profiles
    void loadProfiles();
    void saveProfilesToDisk();
    void showProfilesEditor();
    void createDefaultProfiles();
    QString profilesConfigPath() const;

    void startSshProcess(const QString &target);
    void appendTerminalLine(const QString &line);
    void updatePqStatusLabel(const QString &text, const QString &colorHex);

    QListWidget    *m_profileList    = nullptr;
    QPlainTextEdit *m_terminal       = nullptr;
    QPushButton    *m_connectBtn     = nullptr;
    QPushButton    *m_disconnectBtn  = nullptr;
    QLineEdit      *m_hostField      = nullptr;
    QLabel         *m_statusLabel    = nullptr;

    QLineEdit      *m_inputField     = nullptr;
    QPushButton    *m_sendBtn        = nullptr;

    QProcess       *m_sshProcess     = nullptr;

    QLabel         *m_pqStatusLabel  = nullptr;
    bool            m_pqActive       = false;
    bool probePqSupport(const QString &target);
    void applyTerminalProfile(const SshProfile &p);   // NEW
    QCheckBox      *m_pqDebugCheck   = nullptr;

    QVector<SshProfile> m_profiles;

    QPushButton    *m_editProfilesBtn = nullptr;

    ssh_session m_session = nullptr;   // libssh session for interactive shell
    TerminalView *m_shellView = nullptr;
    QThread *m_shellThread = nullptr;
    SshShellWorker *m_shellWorker = nullptr;
    QTermWidget   *m_colorShell = nullptr;  // full-color terminal window

    bool establishSshSession(const QString &target);
//    QPlainTextEdit *m_shellView = nullptr;
};

#endif // MAINWINDOW_H

