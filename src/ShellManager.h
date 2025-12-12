#pragma once

#include <QObject>
#include <QString>

class QWidget;
class QMainWindow;
class QTabWidget;

class CpunkTermWidget;
struct SshProfile;

class ShellManager : public QObject
{
    Q_OBJECT
public:
    explicit ShellManager(QObject *parent = nullptr);
    ~ShellManager() override;

    // Open a shell (tabbed or separate window)
    void openShell(const SshProfile &profile,
                   const QString &target,
                   bool openInNewWindow,
                   QWidget *parentForWindows);

    // Used for downloads
    QString currentSelectionText() const;

    // Optional helper: send text to active terminal
    bool sendToActiveShell(const QString &text);

    // Close/hide all managed windows
    void closeAll();

    signals:
        // Drag & drop from terminal
        void fileDropped(const QString &path, const QByteArray &data);

private:
    void ensureTabbedWindow(QWidget *anchorWindow);
    CpunkTermWidget* createTerminal(const SshProfile &p, QWidget *parent);
    void applyTerminalProfile(CpunkTermWidget *term, const SshProfile &p);
    void startSshInTerminal(CpunkTermWidget *term, const QString &target, bool pqDebug);
    void setActive(CpunkTermWidget *term);

private:
    CpunkTermWidget *m_activeTerm = nullptr;

    // Tabbed shell window (owned by Qt via parent/WA)
    QMainWindow *m_tabbedWin = nullptr;
    QTabWidget  *m_tabs      = nullptr;
};
