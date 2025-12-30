#include "ShellManager.h"

#include <QMainWindow>
#include <QTabWidget>
#include <QFontDatabase>
#include <QTimer>
#include <QWidget>
#include <QRect>

#include "CpunkTermWidget.h"
#include "SshProfile.h"   // ✅ use the shared profile header (NOT MainWindow.h)
#include <QStringList>
#include "CpunkTermWidget.h"

// ShellManager.cpp
// ---------------
// ShellManager centralizes “terminal window management” for PQ-SSH.
//
// Responsibilities:
// - Create CpunkTermWidget instances configured from an SshProfile (font/scheme).
// - Open shells either in a new window per connection, or in a shared tabbed window.
// - Start an ssh process inside a terminal widget and keep track of the active terminal.
// - Relay user intent upward via signals (e.g., fileDropped from a terminal).
//
// Non-responsibilities:
// - No profile persistence (ProfileStore does that).
// - No SFTP/libssh operations (SshClient + FilesTab do that).
// - No macro expansion or command templating (MainWindow / macro layer does that).
//
// Design notes:
// - Focus handling uses QTimer::singleShot(0, ...) to work around WM timing.
// - Tab close destroys only the tab widget; the tabbed window is hidden and reused.

static QStringList installedTermSchemes()
{
    // Probe qtermwidget via a temporary instance to list available color schemes.
    // This is safe and quick; the instance is not shown.
    CpunkTermWidget probe(0, nullptr);
    auto schemes = probe.availableColorSchemes();
    schemes.removeDuplicates();
    schemes.sort(Qt::CaseInsensitive);
    return schemes;
}

// Construct a ShellManager. It starts with no windows and no active terminal.
ShellManager::ShellManager(QObject *parent)
    : QObject(parent)
{
}

// Destructor ensures shells/windows are closed/hidden and active pointer cleared.
// Note: tabbed window is reused (WA_DeleteOnClose=false), so we hide rather than delete.
ShellManager::~ShellManager()
{
    closeAll();
}

// Ensure the shared tabbed shell window exists.
// If already created, this is a no-op.
void ShellManager::ensureTabbedWindow(QWidget *anchorWindow)
{
    if (m_tabbedWin && m_tabs)
        return;

    // Parent to anchorWindow so it closes with main app (but we keep it hidden/reused)
    m_tabbedWin = new QMainWindow(anchorWindow);
    m_tabbedWin->setWindowTitle(QStringLiteral("CPUNK PQ-SSH – Shells"));
    m_tabbedWin->setAttribute(Qt::WA_DeleteOnClose, false);

    m_tabs = new QTabWidget(m_tabbedWin);
    m_tabs->setTabsClosable(true);
    m_tabs->setDocumentMode(true);

    m_tabbedWin->setCentralWidget(m_tabs);

    // Closing a tab destroys its terminal widget.
    // If the last tab is closed, we hide the shared window and clear active terminal.
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
        if (!m_tabs) return;

        QWidget *w = m_tabs->widget(idx);
        m_tabs->removeTab(idx);
        delete w;

        if (m_tabs->count() == 0 && m_tabbedWin) {
            m_tabbedWin->hide();
            m_activeTerm = nullptr;
        } else if (m_tabs->count() > 0) {
            auto *term = qobject_cast<CpunkTermWidget*>(m_tabs->currentWidget());
            setActive(term);
        }
    });

    // Track active terminal whenever current tab changes.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        if (!m_tabs) return;
        auto *term = qobject_cast<CpunkTermWidget*>(m_tabs->currentWidget());
        setActive(term);
    });
}

// Create a terminal widget and apply profile-specific terminal settings.
// The returned widget is owned by the given parent.
CpunkTermWidget* ShellManager::createTerminal(const SshProfile &p, QWidget *parent)
{
    auto *term = new CpunkTermWidget(0, parent);
    applyTerminalProfile(term, p);

    // Bubble file drop events up to the ShellManager consumer.
    connect(term, &CpunkTermWidget::fileDropped,
            this, &ShellManager::fileDropped);

    return term;
}

// Apply visual terminal settings from SshProfile (font size, scheme, etc.).
// This does not start any process; it only configures presentation.
void ShellManager::applyTerminalProfile(CpunkTermWidget *term, const SshProfile &p)
{
    if (!term) return;

    // Font
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(p.termFontSize > 0 ? p.termFontSize : 11);
    f.setBold(false);                 // ✅ try to avoid bold rendering
    term->setTerminalFont(f);

    // Color scheme
    const QString scheme = p.termColorScheme.isEmpty()
                               ? QStringLiteral("WhiteOnBlack")
                               : p.termColorScheme;
    term->setColorScheme(scheme);

    // Make sure background is applied immediately
    term->update();
}

// Start an ssh session inside an existing terminal widget.
// This uses qtermwidget's process launching (shellProgram + args).
void ShellManager::startSshInTerminal(CpunkTermWidget *term, const QString &target, bool pqDebug)
{
    if (!term) return;

    QStringList args;
    args << "-tt";
    if (pqDebug)
        args << "-vv";

    // Prefer hybrid PQ KEX if server supports it (OpenSSH-style).
    args << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    args << target;

    term->setShellProgram(QStringLiteral("ssh"));
    term->setArgs(args);
    term->startShellProgram();

    setActive(term);

    // Focus after show/activate (critical for "type without clicking")
    QTimer::singleShot(0, term, [term]() {
        term->setFocus(Qt::OtherFocusReason);
    });
}

// Mark a terminal as the currently active one (used for selection + sendText routing).
void ShellManager::setActive(CpunkTermWidget *term)
{
    if (!term) return;
    m_activeTerm = term;
}

// Open a shell for a profile.
// - If newWindow=true: create a dedicated QMainWindow with a single terminal.
// - Else: open in the shared tabbed window (created on demand).
void ShellManager::openShell(const SshProfile &p, const QString &target, bool newWindow, QWidget *anchorWindow)
{
    if (newWindow) {
        auto *win = new QMainWindow(anchorWindow);
        win->setAttribute(Qt::WA_DeleteOnClose, true);
        win->setWindowTitle(QStringLiteral("CPUNK PQ-SSH – %1").arg(target));

        auto *term = createTerminal(p, win);
        win->setCentralWidget(term);

        const int w = (p.termWidth  > 0 ? p.termWidth  : 900);
        const int h = (p.termHeight > 0 ? p.termHeight : 500);
        win->resize(w, h);

        // Place near the anchor window (slightly offset) for better UX.
        if (anchorWindow) {
            QRect r = anchorWindow->geometry();
            win->move(r.center().x() - w/2 + 30, r.center().y() - h/2 + 30);
        }

        win->show();
        win->raise();
        win->activateWindow();

        // extra focus push on the window too
        QTimer::singleShot(0, win, [win, term]() {
            win->raise();
            win->activateWindow();
            term->setFocus(Qt::OtherFocusReason);
        });

        startSshInTerminal(term, target, p.pqDebug);

        // If this window is destroyed, clear active terminal if it was this one.
        connect(win, &QObject::destroyed, this, [this, term]() {
            if (m_activeTerm == term)
                m_activeTerm = nullptr;
        });

        return;
    }

    // Tabbed window mode
    ensureTabbedWindow(anchorWindow);

    auto *term = createTerminal(p, m_tabs);
    const QString tabTitle = !p.name.isEmpty() ? p.name : target;

    const int idx = m_tabs->addTab(term, tabTitle);
    m_tabs->setCurrentIndex(idx);

    const int w = (p.termWidth  > 0 ? p.termWidth  : 900);
    const int h = (p.termHeight > 0 ? p.termHeight : 500);

    if (m_tabbedWin) {
        m_tabbedWin->resize(w, h);

        // First time opening: center relative to anchor.
        if (!m_tabbedWin->isVisible() && anchorWindow) {
            QRect r = anchorWindow->geometry();
            m_tabbedWin->move(r.center().x() - w/2, r.center().y() - h/2);
        }

        m_tabbedWin->show();
        m_tabbedWin->raise();
        m_tabbedWin->activateWindow();

        // focus push
        QTimer::singleShot(0, m_tabbedWin, [this, term]() {
            if (m_tabbedWin) {
                m_tabbedWin->raise();
                m_tabbedWin->activateWindow();
            }
            term->setFocus(Qt::OtherFocusReason);
        });
    }

    startSshInTerminal(term, target, p.pqDebug);
}

// Return the current selection text from the active terminal (if any).
QString ShellManager::currentSelectionText() const
{
    return m_activeTerm ? m_activeTerm->selectedText() : QString();
}

// Send text to the active terminal.
// Returns false if there is no active terminal.
bool ShellManager::sendToActiveShell(const QString &text)
{
    if (!m_activeTerm)
        return false;
    m_activeTerm->sendText(text);
    return true;
}

// Close/hide all shells managed by this instance.
// For the shared tabbed window we hide (so it can be reused); per-window shells
// are owned elsewhere and will close via their own lifecycle.
void ShellManager::closeAll()
{
    if (m_tabbedWin) {
        m_tabbedWin->hide();
    }
    m_activeTerm = nullptr;
}
