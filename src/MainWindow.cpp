#include "MainWindow.h"
#include "KeyGeneratorDialog.h"
#include "KeyMetadataUtils.h"
#include "FilesTab.h"
#include "IdentityManagerDialog.h"
#include <QTextBrowser>
#include <QInputDialog>
#include <QApplication>
#include <QWidget>
#include <QSplitter>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QStatusBar>
#include <QFontDatabase>
#include <QProcess>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QAction>
#include <QPalette>
#include <QDialog>
#include <QDebug>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSysInfo>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>

#include "AppTheme.h"
#include "ProfileStore.h"
#include "ProfilesEditorDialog.h"
#include "SshClient.h"

#include <QTabWidget>
#include <QFont>
#include "CpunkTermWidget.h"
#include <qtermwidget5/qtermwidget.h>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QUrl>
#include "Logger.h"
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <QCryptographicHash>
#include "DilithiumKeyCrypto.h"
#include <sodium.h>
#include <QUuid>
#include <QFrame>
#include "SshConfigImportPlanDialog.h"
#include "SshConfigParser.h"
#include "SshConfigImportPlan.h"

#include <QToolButton>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include "SshConfigImportDialog.h"

#include "SettingsDialog.h"
#include "Fleet/FleetWindow.h"

#include <QCloseEvent>
#include "AuditLogger.h"

//
// ARCHITECTURE NOTES (MainWindow.cpp)
//
// MainWindow is the top-level UI orchestrator. It intentionally *does not*
// implement SSH protocol details, cryptography primitives, or persistent storage.
// Instead it delegates to:
//   - ProfileStore / ProfilesEditorDialog  (profile persistence + edit UI)
//   - SshClient                            (libssh connection, SFTP, server-side key install)
//   - CpunkTermWidget / qtermwidget        (interactive terminal UI)
//   - KeyMetadataUtils / DilithiumKeyCrypto (key metadata + crypto helpers)
//
// This file *does* coordinate cross-cutting UX flows:
//   - connect/disconnect lifecycle
//   - opening terminals (tabs/windows) and wiring their lifetime to sessions
//   - debug-gated diagnostics
//   - key install confirmations and UI safety checks
//
// Guiding rule: keep blocking operations off the UI thread.
// QtConcurrent + QFutureWatcher is used for libssh connects,
// and QProcess is used for external `ssh` probing / terminal sessions.
//

// =====================================================
// Forward declarations + small UI helpers
// =====================================================
//
// These are file-local helpers to keep MainWindow methods focused on flow.
// They should remain “dumb UI glue”: no SSH logic, no crypto, no persistence.
//

class CpunkTermWidget;
static void forceBlackBackground(CpunkTermWidget *term);
static void protectTermFromAppStyles(CpunkTermWidget *term);

// Focus quirks: terminals sometimes need delayed focus to become type-ready.
// Using two singleShots is a pragmatic workaround for window manager timing.
static void focusTerminalWindow(QWidget *window, QWidget *termWidget)
{
    if (!window || !termWidget) return;

    window->show();
    window->raise();
    window->activateWindow();

    QTimer::singleShot(0, window, [termWidget]() {
        termWidget->setFocus(Qt::OtherFocusReason);
    });

    QTimer::singleShot(50, window, [termWidget]() {
        termWidget->setFocus(Qt::OtherFocusReason);
    });
}

// Helper to enforce background colors on containers so terminal does not show “gutters”
// caused by Qt widget backgrounds or style sheets bleeding into parents.
static void applyBg(QWidget* w, const QColor& bg)
{
    if (!w) return;
    w->setAutoFillBackground(true);
    QPalette p = w->palette();
    p.setColor(QPalette::Window, bg);
    w->setPalette(p);
    w->setStyleSheet(QString("background:%1;").arg(bg.name()));
}

// Terminal background inference: qtermwidget uses palette roles; we attempt to
// derive a stable background color for surrounding widgets.
static QColor guessTermBg(QWidget* term)
{
    if (!term) return QColor(0,0,0);

    // Prefer Base if it's meaningful, else Window.
    const QColor base = term->palette().color(QPalette::Base);
    if (base.isValid() && base.alpha() > 0) return base;

    const QColor win = term->palette().color(QPalette::Window);
    if (win.isValid() && win.alpha() > 0) return win;

    return QColor(0,0,0);
}

// Keep the container + tab page synced with terminal background.
// This is purely visual; does not affect terminal content rendering.
static void syncTerminalSurroundingsToTerm(CpunkTermWidget* term)
{
    if (!term) return;

    const QColor bg = guessTermBg(term);

    // terminal itself
    applyBg(term, bg);

    // immediate parent and one level up (tab page / container)
    QWidget* p1 = term->parentWidget();
    applyBg(p1, bg);

    if (p1) {
        QWidget* p2 = p1->parentWidget();
        applyBg(p2, bg);
    }
}

// =====================================================
// Profile list grouping helpers
// =====================================================

static QString normalizedGroupName(const QString& g)
{
    const QString s = g.trimmed();
    return s.isEmpty() ? QStringLiteral("Ungrouped") : s;
}

static bool isGroupHeaderItem(const QListWidgetItem* it)
{
    if (!it) return false;
    return (it->data(Qt::UserRole).toInt() == -1);
}

void MainWindow::rebuildProfileList()
{
    if (!m_profileList) return;

    m_profileList->clear();

    // Build sorted order of indices (so we don't reorder m_profiles storage)
    QVector<int> order;
    order.reserve(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) order.push_back(i);

    std::sort(order.begin(), order.end(), [this](int ia, int ib) {
        const SshProfile& a = m_profiles[ia];
        const SshProfile& b = m_profiles[ib];

        const QString ga = normalizedGroupName(a.group).toLower();
        const QString gb = normalizedGroupName(b.group).toLower();
        if (ga != gb) return ga < gb;

        return a.name.toLower() < b.name.toLower();
    });

    QString currentGroup;
    for (int idx : order) {
        const SshProfile& p = m_profiles[idx];
        const QString g = normalizedGroupName(p.group);

        if (g != currentGroup) {
            currentGroup = g;

            auto* hdr = new QListWidgetItem(currentGroup.toUpper(), m_profileList);
            hdr->setFlags(Qt::NoItemFlags);              // non-selectable
            hdr->setData(Qt::UserRole, -1);              // header marker
            hdr->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            hdr->setSizeHint(QSize(hdr->sizeHint().width(), 26));
            // Optional visual emphasis; keep it subtle so AppTheme can still style
            QSettings s;
            const QString themeId = s.value("ui/theme", "cpunk-dark").toString();
            hdr->setForeground(QBrush(AppTheme::accent(themeId)));
        }

        auto* it = new QListWidgetItem("    " + p.name, m_profileList);
        it->setData(Qt::UserRole, idx); // <-- REAL index into m_profiles
        it->setToolTip(QString("%1@%2:%3  [%4]")
                           .arg(p.user, p.host)
                           .arg(p.port > 0 ? p.port : 22)
                           .arg(normalizedGroupName(p.group)));
    }

    // Select first real profile item
    for (int r = 0; r < m_profileList->count(); ++r) {
        QListWidgetItem* it = m_profileList->item(r);
        if (it && !isGroupHeaderItem(it)) {
            m_profileList->setCurrentRow(r);
            ensureProfileItemSelected();
            const int pidx = currentProfileIndex();
            if (pidx >= 0) onProfileSelectionChanged(pidx); // see updated handler below
            break;
        }
    }
}



// Returns the selected profile index in m_profiles, or -1 if none / header selected.
int MainWindow::currentProfileIndex() const
{
    if (!m_profileList) return -1;
    QListWidgetItem* it = m_profileList->currentItem();
    if (!it) return -1;

    const int idx = it->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_profiles.size()) return -1;
    return idx;
}

// If user clicks a header, move selection to nearest real profile item.
void MainWindow::ensureProfileItemSelected()
{
    if (!m_profileList) return;

    QListWidgetItem* it = m_profileList->currentItem();
    if (!it) return;

    if (!isGroupHeaderItem(it)) return;

    const int row = m_profileList->currentRow();

    // try next items
    for (int r = row + 1; r < m_profileList->count(); ++r) {
        QListWidgetItem* cand = m_profileList->item(r);
        if (cand && !isGroupHeaderItem(cand)) {
            m_profileList->setCurrentRow(r);
            return;
        }
    }
    // try previous items
    for (int r = row - 1; r >= 0; --r) {
        QListWidgetItem* cand = m_profileList->item(r);
        if (cand && !isGroupHeaderItem(cand)) {
            m_profileList->setCurrentRow(r);
            return;
        }
    }
}

void MainWindow::applyCurrentTheme()
{
    QSettings s;
    const QString themeId = s.value("ui/theme", "cpunk-dark").toString();

    qApp->setStyleSheet("");
    qApp->setPalette(QApplication::style()->standardPalette());

    if (themeId == "cpunk-orange") {
        qApp->setStyleSheet(AppTheme::orange());
    } else if (themeId == "windows-basic") {
        qApp->setStyleSheet(AppTheme::windowsBasic());
    } else if (themeId == "cpunk-neo") {                 // NEW
        qApp->setStyleSheet(AppTheme::neo());            // NEW
    } else {
        qApp->setStyleSheet(AppTheme::dark());
    }

    rebuildProfileList();  // refresh header brushes
}



// =====================================================
// MainWindow lifecycle
// =====================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // ---- Startup logs (to pq-ssh.log via Logger::install in main.cpp) ----
    // Rationale: keep a stable environment fingerprint for debugging user reports.
    qInfo() << "MainWindow constructing";
    qInfo() << "App:" << QCoreApplication::applicationName()
            << "Version:" << QCoreApplication::applicationVersion();
    qInfo() << "Qt:" << QT_VERSION_STR
            << "OS:" << QSysInfo::prettyProductName()
            << "Platform:" << QGuiApplication::platformName();

    // Global widget theme. Terminal colors are handled separately.
    applySavedSettings();

    setWindowTitle("CPUNK PQ-SSH");
    resize(1200, 700);

    // UI-first: build widgets, then load profiles, then wire menus/actions.
    // Profiles may affect initial UI state (host field, debug checkbox).
    setupUi();
    loadProfiles();      // loadProfiles() calls rebuildProfileList()
    setupMenus();

    // Passphrase prompt provider for libssh when an OpenSSH private key is encrypted.
    // Architectural note: MainWindow supplies *UI* for passphrase prompts,
    // while SshClient owns the actual key loading logic.
    m_ssh.setPassphraseProvider([this](const QString& keyFile, bool *ok) -> QString {
        const QString title = "SSH Key Passphrase";
        const QString label = keyFile.trimmed().isEmpty()
            ? QString("Enter passphrase for private key:")
            : QString("Enter passphrase for key:\n%1").arg(QFileInfo(keyFile).fileName());

        bool localOk = false;
        const QString pass = QInputDialog::getText(
            this,
            title,
            label,
            QLineEdit::Password,
            QString(),
            &localOk
        );

        if (ok) *ok = localOk;
        return pass;
    });

    qInfo() << "UI ready; profiles loaded";

    // ---- Startup security warning: expired keys ----
    // This is a user-facing safety net:
    //   1) auto-mark expired keys (metadata repair)
    //   2) show an explicit warning if any keys are expired
    //
    // This does NOT modify private keys; it only updates metadata.json.
    {
        const QString metaPath = QDir(QDir::homePath()).filePath(".pq-ssh/keys/metadata.json");
        qInfo() << "Checking key metadata:" << metaPath;

        // 1) Keep metadata consistent by auto-expiring outdated entries.
        QString autoErr;
        autoExpireMetadataFile(metaPath, &autoErr);

        if (!autoErr.isEmpty()) {
            appendTerminalLine("[WARN] " + autoErr);
            qWarning() << "autoExpireMetadataFile:" << autoErr;
        }

        // 2) Count expired keys after auto-marking.
        QString e;
        const int expired = countExpiredKeysInMetadata(&e);

        if (!e.isEmpty()) {
            appendTerminalLine("[WARN] " + e);
            qWarning() << "countExpiredKeysInMetadata:" << e;
        }

        qInfo() << "Expired keys:" << expired;

        if (expired > 0) {
            const QString msg =
                QString("⚠ WARNING: You have %1 expired SSH key(s). Open Key Generator → Keys tab to review/rotate.")
                    .arg(expired);

            // Status label gives persistent, visible warning in the main window.
            if (m_statusLabel) {
                m_statusLabel->setText(msg);
                m_statusLabel->setStyleSheet("color: #ff5252; font-weight: bold;");
            }

            // Terminal log provides an audit trail (and ends up in the file log too).
            appendTerminalLine("[SECURITY] " + msg);
            qWarning() << "SECURITY:" << msg;
        }
    }

    qInfo() << "MainWindow constructed OK";
}

MainWindow::~MainWindow()
{
    // Defensive: ensure background sessions are closed on app exit.
    // (The terminal sessions launched via QTermWidget may also exit independently.)
    m_ssh.disconnect();
}

// ========================
// UI construction
// ========================
//
// setupUi() builds the widget tree and wires signals/slots.
// It should not contain business logic (SSH/crypto), only view assembly.
//

void MainWindow::setupUi()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1); // thinner handle
    setCentralWidget(splitter);

    // ============================
    // Left pane: profile list
    // ============================
    auto *profilesWidget = new QWidget(splitter);
    auto *profilesLayout = new QVBoxLayout(profilesWidget);
    profilesLayout->setContentsMargins(8, 8, 8, 8);
    profilesLayout->setSpacing(6);

    auto *profilesLabel = new QLabel("Profiles", profilesWidget);
    profilesLabel->setStyleSheet("font-weight: bold;");

    m_profileList = new QListWidget(profilesWidget);
    m_profileList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_editProfilesBtn = new QPushButton("Edit profiles…", profilesWidget);
    m_editProfilesBtn->setToolTip("Manage profiles inside the app");

    profilesLayout->addWidget(profilesLabel);
    profilesLayout->addWidget(m_profileList, 1);
    profilesLayout->addWidget(m_editProfilesBtn, 0);
    profilesWidget->setLayout(profilesLayout);

    // ============================
    // Right pane: connect controls + terminal + status
    // ============================
    auto *rightWidget = new QWidget(splitter);

    // Outer layout keeps padding for top/input/bottom bars
    auto *outer = new QVBoxLayout(rightWidget);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    // --- Top bar: connection target + buttons ---
    auto *topBar = new QWidget(rightWidget);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);

    auto *hostLabel = new QLabel("Host:", topBar);
    m_hostField = new QLineEdit(topBar);
    m_hostField->setPlaceholderText("user@hostname");

    m_connectBtn = new QPushButton("Connect", topBar);
    m_disconnectBtn = new QPushButton("Disconnect", topBar);
    m_disconnectBtn->setEnabled(false);


    topLayout->addWidget(hostLabel);
    topLayout->addWidget(m_hostField, 1);
    topLayout->addWidget(m_connectBtn);
    topLayout->addWidget(m_disconnectBtn);
    topBar->setLayout(topLayout);

    // --- Terminal container ---
    // Note: m_terminal is currently a QPlainTextEdit “log view”.
    // Real interactive terminals are created on connect via CpunkTermWidget.
    // Keeping a simple log view here helps debugging and provides a consistent
    // place for status text even when terminals open in separate windows.
    // --- Terminal container -> now a QTabWidget with Log + Files ---
    auto *termContainer = new QWidget(rightWidget);
    auto *termLayout = new QVBoxLayout(termContainer);
    termLayout->setContentsMargins(0, 0, 0, 0);
    termLayout->setSpacing(0);

    m_mainTabs = new QTabWidget(termContainer);
    m_mainTabs->setDocumentMode(true);

    auto *logPage = new QWidget(m_mainTabs);
    auto *logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(0);

    m_terminal = new QPlainTextEdit(logPage);
    m_terminal->setReadOnly(true);
    m_terminal->setFrameShape(QFrame::NoFrame);
    m_terminal->setLineWidth(0);
    m_terminal->setContentsMargins(0, 0, 0, 0);
    m_terminal->document()->setDocumentMargin(0);
    m_terminal->viewport()->setAutoFillBackground(true);
    m_terminal->viewport()->setContentsMargins(0, 0, 0, 0);

    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(11);
    m_terminal->setFont(terminalFont);

    logLayout->addWidget(m_terminal, 1);
    logPage->setLayout(logLayout);

    m_filesTab = new FilesTab(&m_ssh, m_mainTabs);

    m_mainTabs->addTab(logPage, "Log");
    m_mainTabs->addTab(m_filesTab, "Files");

    termLayout->addWidget(m_mainTabs, 1);
    termContainer->setLayout(termLayout);

    // --- Input bar ---
    // Architectural note: input bar is currently not wired to a shell.
    // It exists as a UI placeholder and can later become:
    //   - a quick “send command” feature (non-interactive), or
    //   - a fallback command runner when no terminal is open.
    auto *inputBar = new QWidget(rightWidget);
    auto *inputLayout = new QHBoxLayout(inputBar);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(6);

    m_inputField = new QLineEdit(inputBar);
    m_inputField->setPlaceholderText("Type command (not wired yet) ...");

    m_sendBtn = new QPushButton("Send", inputBar);
    m_sendBtn->setEnabled(true);

    inputLayout->addWidget(m_inputField, 1);
    inputLayout->addWidget(m_sendBtn);
    inputBar->setLayout(inputLayout);

    // --- Bottom bar: status + debug controls ---
    auto *bottomBar = new QWidget(rightWidget);
    auto *bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);

    m_statusLabel = new QLabel("Ready.", bottomBar);
    m_statusLabel->setStyleSheet("color: gray;");

    // PQ status is derived from an OpenSSH probe (sntrup761 hybrid KEX check).
    m_pqStatusLabel = new QLabel("PQ: unknown", bottomBar);
    m_pqStatusLabel->setStyleSheet("color: #888; font-weight: bold;");

    // PQ debug gates *UI verbosity* and enables dev/test actions.
    // Important: debug mode should not silently alter security behavior—only visibility.
    m_pqDebugCheck = new QCheckBox("PQ debug", bottomBar);
    m_pqDebugCheck->setChecked(true);

    // Connection UX: choose between new window per session or tabbed sessions.
    m_openInNewWindowCheck = new QCheckBox("Open new connection in NEW window", bottomBar);
    m_openInNewWindowCheck->setChecked(true);

    bottomLayout->addWidget(m_statusLabel, 1);
    bottomLayout->addWidget(m_openInNewWindowCheck, 0);
    bottomLayout->addWidget(m_pqStatusLabel, 0);
    bottomLayout->addWidget(m_pqDebugCheck, 0);
    bottomBar->setLayout(bottomLayout);

    // Assemble right side
    outer->addWidget(topBar);
    outer->addWidget(termContainer, 1);
    outer->addWidget(inputBar, 0);
    outer->addWidget(bottomBar);

    // Add panes to splitter
    splitter->addWidget(profilesWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // Optional: ensure splitter handle isn't black in dark theme
    splitter->setStyleSheet("QSplitter::handle { background-color: #121212; }");

    // ============================
    // Wiring (signals/slots)
    // ============================
    // These are UI-level connections that delegate to handler methods.
    connect(m_connectBtn, &QPushButton::clicked,
            this, &MainWindow::onConnectClicked);

    connect(m_disconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onDisconnectClicked);

    connect(m_profileList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onProfileDoubleClicked(); });

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &MainWindow::onProfileSelectionChanged);

    connect(m_sendBtn, &QPushButton::clicked,
            this, &MainWindow::onSendInput);

    connect(m_inputField, &QLineEdit::returnPressed,
            this, &MainWindow::onSendInput);

    connect(m_editProfilesBtn, &QPushButton::clicked,
            this, &MainWindow::onEditProfilesClicked);

    connect(&m_ssh, &SshClient::kexNegotiated, this,
            [this](const QString& pretty, const QString& raw) {

                const bool pq =
                    raw.contains("mlkem", Qt::CaseInsensitive) ||
                    raw.contains("sntrup", Qt::CaseInsensitive);

                // ✅ THIS is the exact place for the conditional message
                if (pq) {
                    appendTerminalLine(
                        QString("[PQ] 🧬 Post-Quantum key exchange established → %1")
                            .arg(pretty)
                    );
                } else {
                    appendTerminalLine(
                        QString("[KEX] Classical key exchange negotiated → %1")
                            .arg(pretty)
                    );
                }

                // Bottom/status bar (short & neutral)
                updatePqStatusLabel(
                    pq ? ("Post-Quantum KEX: " + pretty) : ("KEX: " + pretty),
                    pq ? "#00FF99" : "#9AA0A6"
                );

                // Tooltip with raw negotiated algorithm
                if (m_pqStatusLabel)
                    m_pqStatusLabel->setToolTip("Negotiated KEX: " + raw);
            });
}

void MainWindow::setupMenus()
{
    // ARCHITECTURE:
    // Menus are the “command surface” of the app. MainWindow wires actions to
    // higher-level workflows, but actual work is delegated:
    //   - Key generation UI -> KeyGeneratorDialog
    //   - Server-side key install -> SshClient::installAuthorizedKey(...)
    //   - DEV crypto checks -> SshClient / DilithiumKeyCrypto helpers
    //
    // Design principle: keep menu handlers thin; validate input in UI,
    // then call a single workflow function (e.g. onInstallPublicKeyRequested).

    // ----------------------------
    // File
    // ----------------------------
    auto *fileMenu = menuBar()->addMenu("&File");

    QAction *settingsAct = fileMenu->addAction("Settings…");
    settingsAct->setToolTip("Open PQ-SSH settings");
    connect(settingsAct, &QAction::triggered, this, [this]() {

        // If already open, bring to front
        if (m_settingsDlg) {
            m_settingsDlg->raise();
            m_settingsDlg->activateWindow();
            return;
        }

        // IMPORTANT: parent = nullptr so it’s freely movable and not “attached”
        m_settingsDlg = new SettingsDialog(nullptr);
        m_settingsDlg->setAttribute(Qt::WA_DeleteOnClose, true);
        m_settingsDlg->setWindowTitle("CPUNK PQ-SSH — Settings");

        // When closed, clear pointer
        connect(m_settingsDlg, &QObject::destroyed, this, [this]() {
            m_settingsDlg = nullptr;
        });

        // Apply settings when user presses OK/Apply (assuming Accepted exists)
        connect(m_settingsDlg, &QDialog::accepted, this, [this]() {
            applySavedSettings();
            rebuildProfileList();
            appendTerminalLine("[INFO] Settings updated.");
            if (m_statusLabel) m_statusLabel->setText("Settings updated.");
        });

        m_settingsDlg->show();
        m_settingsDlg->raise();
        m_settingsDlg->activateWindow();
    });

    QAction *importSshConfigAct = fileMenu->addAction("Import OpenSSH config…");
    importSshConfigAct->setToolTip("Read ~/.ssh/config and preview entries (no profiles are created yet).");
    connect(importSshConfigAct, &QAction::triggered, this, &MainWindow::onImportOpenSshConfig);

    fileMenu->addSeparator();

    QAction *quitAct = fileMenu->addAction("Quit");
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    // ----------------------------
    // Tools (Fleet jobs)
    // ----------------------------
    auto *toolsMenu = menuBar()->addMenu("&Tools");

    // If already open, bring to front (modeless window)
    static QPointer<FleetWindow> g_fleetWin;

    QAction *fleetAct = toolsMenu->addAction("Fleet jobs…");
    fleetAct->setToolTip("Run the same job across multiple profiles/hosts (e.g., deploy to 10 servers).");

    connect(fleetAct, &QAction::triggered, this, [this]() {
        if (g_fleetWin) {
            g_fleetWin->raise();
            g_fleetWin->activateWindow();
            return;
        }

        // IMPORTANT: parent = nullptr so it's freely movable and not “attached”
        g_fleetWin = new FleetWindow(m_profiles, nullptr);
        g_fleetWin->setAttribute(Qt::WA_DeleteOnClose, true);

        // When closed, clear pointer
        connect(g_fleetWin, &QObject::destroyed, this, [&]() {
            g_fleetWin = nullptr;
        });

        g_fleetWin->show();
        g_fleetWin->raise();
        g_fleetWin->activateWindow();
    });


    // ----------------------------
    // Keys
    // ----------------------------
    auto *keysMenu = menuBar()->addMenu("&Keys");

    QAction *keyGenAct = new QAction("Key Generator...", this);
    keyGenAct->setToolTip("Generate keys and optionally install the public key to a server profile");
    keysMenu->addAction(keyGenAct);

    connect(keyGenAct, &QAction::triggered, this, [this]() {

        // If already open, bring to front
        if (m_keyGenerator) {
            m_keyGenerator->raise();
            m_keyGenerator->activateWindow();
            return;
        }

        // Build fresh profile name list (always up-to-date)
        QStringList names;
        names.reserve(m_profiles.size());
        for (const auto &p : m_profiles)
            names << p.name;

        // IMPORTANT: parent = nullptr so it’s freely movable and not “attached”
        m_keyGenerator = new KeyGeneratorDialog(names, nullptr);
        m_keyGenerator->setAttribute(Qt::WA_DeleteOnClose, true);
        m_keyGenerator->setWindowTitle("CPUNK PQ-SSH — Key Generator");

        // Keep existing workflow wiring
        connect(m_keyGenerator, &KeyGeneratorDialog::installPublicKeyRequested,
                this, &MainWindow::onInstallPublicKeyRequested);

        // When closed, clear pointer
        connect(m_keyGenerator, &QObject::destroyed, this, [this]() {
            m_keyGenerator = nullptr;
        });

        m_keyGenerator->show();
        m_keyGenerator->raise();
        m_keyGenerator->activateWindow();
    });

    // Menu-based installer (manual .pub selection).
    // ARCHITECTURE:
    // This is a convenience entry point to the same underlying workflow:
    // onInstallPublicKeyRequested(pubKeyLine, profileIndex)
    // Keeping the workflow centralized ensures consistent confirmations,
    // idempotence, and error handling regardless of entry point.
    QAction *installPubAct = new QAction("Install public key to server…", this);
    installPubAct->setToolTip("Append an OpenSSH public key to ~/.ssh/authorized_keys on the selected profile host.");
    keysMenu->addAction(installPubAct);

    connect(installPubAct, &QAction::triggered, this, [this]() {
        ensureProfileItemSelected();
        const int row = currentProfileIndex();
        if (row < 0 || row >= m_profiles.size()) {
            QMessageBox::warning(this, "Key install", "No profile selected.");
            return;
        }

        // UI responsibility: choose and read a public key file.
        // No remote operations happen until the central workflow is called.
        const QString pubPath = QFileDialog::getOpenFileName(
            this,
            "Select OpenSSH public key (.pub)",
            QDir::homePath() + "/.ssh",
            "Public keys (*.pub);;All files (*)"
        );
        if (pubPath.isEmpty()) return;

        QFile f(pubPath);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, "Key install failed", "Cannot read file:\n" + f.errorString());
            return;
        }
        const QString pubLine = QString::fromUtf8(f.readAll()).trimmed();
        f.close();

        // Centralized key install flow (confirmation + SshClient call).
        onInstallPublicKeyRequested(pubLine, row);
    });

    // DEV/Tester: hidden behind PQ debug
    // ARCHITECTURE:
    // Debug-gated actions allow internal diagnostics without exposing them
    // to normal users. This avoids UI clutter and reduces risk of users
    // triggering dev-only crypto helpers.
    QAction *testUnlockAct = new QAction("DEV: Test Dilithium key unlock…", this);
    testUnlockAct->setToolTip("Dev tool: decrypt .enc key and validate format (only visible when PQ debug is ON).");
    keysMenu->addAction(testUnlockAct);

    auto syncDevVisibility = [this, testUnlockAct]() {
        const bool on = (m_pqDebugCheck && m_pqDebugCheck->isChecked());
        testUnlockAct->setVisible(on);
        testUnlockAct->setEnabled(on);
    };
    syncDevVisibility();

    if (m_pqDebugCheck) {
        // Keep visibility consistent with the checkbox state at runtime.
        connect(m_pqDebugCheck, &QCheckBox::toggled, this, [syncDevVisibility](bool) {
            syncDevVisibility();
        });
    }

    connect(testUnlockAct, &QAction::triggered, this, [this]() {
        // Safety gate (in case someone triggers it via shortcut later)
        if (!(m_pqDebugCheck && m_pqDebugCheck->isChecked())) {
            appendTerminalLine("[DEV] PQ debug is OFF. Enable it to use the tester.");
            return;
        }

        // ARCHITECTURE:
        // This tool intentionally avoids revealing secrets:
        //   - it prints sizes and hashes
        //   - it does not print decrypted plaintext
        // It also does basic format checks before trying decryption.
        const QString path = QFileDialog::getOpenFileName(
            this,
            "Select encrypted Dilithium key (.enc)",
            QDir(QDir::homePath()).filePath(".pq-ssh/keys"),
            "Encrypted keys (*.enc);;All files (*)"
        );
        if (path.isEmpty()) return;

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            appendTerminalLine(QString("[TEST] ❌ Cannot read file: %1").arg(f.errorString()));
            return;
        }
        const QByteArray enc = f.readAll();
        f.close();

        appendTerminalLine(QString("[TEST] File: %1").arg(QFileInfo(path).fileName()));
        appendTerminalLine(QString("[TEST] Enc size: %1 bytes").arg(enc.size()));

        // Minimal structure check: MAGIC(6)+SALT(16)+NONCE(24)+TAG(16) at least
        const int minSize =
            6 + crypto_pwhash_SALTBYTES + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES +
            crypto_aead_xchacha20poly1305_ietf_ABYTES;

        if (enc.size() < minSize) {
            appendTerminalLine(QString("[TEST] ❌ Too small to be valid (need >= %1 bytes)").arg(minSize));
            return;
        }

        const QByteArray magic = enc.left(6);
        appendTerminalLine(QString("[TEST] Magic: '%1'").arg(QString::fromLatin1(magic)));
        if (magic != "PQSSH1") {
            appendTerminalLine("[TEST] ⚠ WARN: Magic mismatch (expected 'PQSSH1'). Will still try decrypt.");
        }

        // Delegation: decryption/test logic lives in SshClient/DilithiumKeyCrypto,
        // not here in the UI layer.
        QString err;
        const bool ok = m_ssh.testUnlockDilithiumKey(path, &err);
        if (ok) {
            appendTerminalLine(QString("[TEST] ✅ Unlock OK: %1").arg(QFileInfo(path).fileName()));

            // Optional: compute SHA256 of encrypted blob (useful to compare files).
            // Hashing ciphertext is safe and helps with bug reports.
            const QByteArray encSha = QCryptographicHash::hash(enc, QCryptographicHash::Sha256).toHex();
            appendTerminalLine(QString("[TEST] Enc SHA256: %1").arg(QString::fromLatin1(encSha)));
        } else {
            appendTerminalLine(QString("[TEST] ❌ Unlock FAILED: %1").arg(err));
        }
    });

    // ----------------------------
    // View (presentation + auxiliary windows)
    // ----------------------------
    auto *viewMenu = menuBar()->addMenu("&View");

    QAction *identityAct = new QAction("Identity manager…", this);
    identityAct->setToolTip("Recover a global SSH keypair from 24 words (Ed25519).");
    viewMenu->addAction(identityAct);
    connect(identityAct, &QAction::triggered, this, &MainWindow::onIdentityManagerRequested);

    // ----------------------------
    // Help
    // ----------------------------
    auto *helpMenu = menuBar()->addMenu("&Help");

    QAction *manualAct = new QAction("User Manual", this);
    manualAct->setToolTip("Open PQ-SSH user manual");
    connect(manualAct, &QAction::triggered, this, &MainWindow::onOpenUserManual);
    helpMenu->addAction(manualAct);

    QAction *openLogAct = new QAction("Open log file", this);
    openLogAct->setToolTip("Open pq-ssh log file");
    connect(openLogAct, &QAction::triggered, this, &MainWindow::onOpenLogFile);
    helpMenu->addAction(openLogAct);

    statusBar()->showMessage("CPUNK PQ-SSH prototype");
}




void MainWindow::appendTerminalLine(const QString &line)
{
    // ARCHITECTURE:
    // appendTerminalLine() is the UI “log sink” for user-visible messages.
    // It intentionally hides certain high-volume diagnostics unless PQ debug is enabled.
    // Even when hidden from the UI, messages are still written to the file log via qInfo().
    const bool verbose = (m_pqDebugCheck && m_pqDebugCheck->isChecked());

    // Hide noisy diagnostics in normal mode
    if (!verbose) {
        if (line.startsWith("[SSH-CMD]") ||
            line.startsWith("[SSH-WRAP]") ||
            line.startsWith("[TERM] available schemes:") ||
            line.startsWith("[TERM] profile=") ||
            line.startsWith("[SECURITY] Local shell fallback")) {
            // Still log to file for debugging
            qInfo().noquote() << "[UI-HIDDEN]" << line;
            return;
        }
    }

    // UI side: only the main window log view is appended here.
    // Terminals have their own output streams.
    if (m_terminal)
        m_terminal->appendPlainText(line);
}

void MainWindow::updatePqStatusLabel(const QString &text, const QString &colorHex)
{
    // ARCHITECTURE:
    // PQ status is a best-effort indicator derived from an OpenSSH probe,
    // not from the libssh session. It is intentionally lightweight and non-blocking.
    if (!m_pqStatusLabel) return;

    m_pqStatusLabel->setText(text);

    QString col = colorHex.trimmed();
    if (col.isEmpty()) {
        // Theme accent (so CPUNK Dark/Orange/Windows Basic can control it)
        col = qApp->palette().color(QPalette::Highlight).name();
    }

    m_pqStatusLabel->setStyleSheet(QString("color:%1; font-weight:bold;").arg(col));
}


// ========================
// Profiles
// ========================
//
// Profile persistence lives in ProfileStore. MainWindow owns the in-memory list
// and updates the UI. Any edits flow through ProfilesEditorDialog.
//

void MainWindow::loadProfiles()
{
    QString err;
    m_profiles = ProfileStore::load(&err);

    // If there are no profiles, seed defaults and persist them.
    // This gives first-time users a usable starting point.
    if (m_profiles.isEmpty()) {
        m_profiles = ProfileStore::defaults();
        QString saveErr;
        ProfileStore::save(m_profiles, &saveErr);
        if (!saveErr.isEmpty())
            appendTerminalLine("[WARN] Could not save default profiles: " + saveErr);
    }

    rebuildProfileList();

    // Non-fatal load warning goes to UI log.
    if (!err.isEmpty())
        appendTerminalLine("[WARN] ProfileStore: " + err);
}

void MainWindow::saveProfilesToDisk()
{
    // ARCHITECTURE:
    // Saving is centralized so all profile edits share the same failure handling.
    QString err;
    if (!ProfileStore::save(m_profiles, &err)) {
        appendTerminalLine("[ERROR] " + err);
        if (m_statusLabel)
            m_statusLabel->setText("Failed to save profiles");
    }
}

void MainWindow::onEditProfilesClicked()
{
    // If already open, just bring it to front.
    if (m_profilesEditor) {
        m_profilesEditor->raise();
        m_profilesEditor->activateWindow();
        return;
    }

    ensureProfileItemSelected();
    const int selected = currentProfileIndex();

    // Create modeless dialog (floating window, main window still usable)
    m_profilesEditor = new ProfilesEditorDialog(m_profiles, (selected >= 0 ? selected : 0), nullptr);
    m_profilesEditor->setAttribute(Qt::WA_DeleteOnClose, true);
    m_profilesEditor->setModal(false);
    m_profilesEditor->setWindowModality(Qt::NonModal);

    // Optional: keep it as a real top-level window even if Qt tries to parent it
    m_profilesEditor->setWindowFlag(Qt::Window, true);

    // When user clicks Save (Accepted), apply and persist
    connect(m_profilesEditor, &QDialog::accepted, this, [this]() {
        if (!m_profilesEditor) return;

        m_profiles = m_profilesEditor->resultProfiles();
        saveProfilesToDisk();
        rebuildProfileList();

        if (m_statusLabel)
            m_statusLabel->setText("Profiles updated.");
        appendTerminalLine("[INFO] Profiles updated.");
    });

    // When it closes (Save or Cancel), clear pointer
    connect(m_profilesEditor, &QObject::destroyed, this, [this]() {
        m_profilesEditor = nullptr;
    });

    m_profilesEditor->show();
    m_profilesEditor->raise();
    m_profilesEditor->activateWindow();
}


void MainWindow::onInstallPublicKeyRequested(const QString& pubKeyLine, int profileIndex)
{
    // ARCHITECTURE:
    // This is the single “source of truth” workflow for server-side key install.
    // Entry points:
    //   - KeyGeneratorDialog (install selected key)
    //   - Menu action (select .pub file)
    //
    // Responsibilities:
    //   - UI validation + confirmation (host/user/port + key preview)
    //   - Ensure an SSH(SFTP) session exists (via SshClient)
    //   - Delegate idempotent authorized_keys modification to SshClient

    if (pubKeyLine.trimmed().isEmpty()) {
        QMessageBox::warning(this, "Key install", "Public key is empty.");
        return;
    }

    if (profileIndex < 0 || profileIndex >= m_profiles.size()) {
        QMessageBox::warning(this, "Key install", "Invalid target profile.");
        return;
    }

    const SshProfile &p = m_profiles[profileIndex];

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        QMessageBox::warning(this, "Key install", "Profile has empty user/host.");
        return;
    }

    const int port = (p.port > 0) ? p.port : 22;

    const QString hostLine =
        (port != 22)
            ? QString("%1@%2:%3").arg(p.user, p.host).arg(port)
            : QString("%1@%2").arg(p.user, p.host);

    // UX: show a safe preview (type + truncated key blob), not the whole line.
    // This avoids accidental full-key disclosure in screenshots/logs.
    const QStringList parts =
        pubKeyLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const QString keyType = parts.size() >= 1 ? parts[0] : QString("unknown");
    const QString keyBlob = parts.size() >= 2 ? parts[1] : QString();
    const QString preview =
        keyBlob.isEmpty() ? QString() : (keyBlob.left(18) + "…" + keyBlob.right(10));

    const QString confirmText =
        "You are about to install a public key to this host:\n\n"
        "Target profile:\n  " + p.name + "\n\n"
        "Host:\n  " + hostLine + "\n\n"
        "Remote path:\n  ~/.ssh/authorized_keys\n\n"
        "Key type:\n  " + keyType + "\n\n"
        "Key preview:\n  " + preview + "\n\n"
        "Proceed?";

    auto btn = QMessageBox::question(
        this,
        "Confirm key install",
        confirmText,
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel
    );

    if (btn != QMessageBox::Yes)
        return;

    // Ensure we have a libssh session (SFTP) to that host.
    // Note: this may prompt for password / passphrase via provider.
    if (!m_ssh.isConnected()) {
        QString e;
        if (!m_ssh.connectProfile(p, &e)) {
            QMessageBox::critical(this, "Key install failed",
                                  "SSH(SFTP) connection failed:\n" + e);
            return;
        }
    }

    // Delegation: SshClient performs an idempotent install (backup + append-if-missing).
    QString err;
    bool already = false;

    if (!m_ssh.installAuthorizedKey(pubKeyLine, &err, &already)) {
        QMessageBox::critical(this, "Key install failed", err);
        return;
    }

    QMessageBox::information(
        this,
        "Key install",
        already ? "Key already existed in authorized_keys."
                : "Key installed successfully."
    );
}


// ========================
// Connect / disconnect
// ========================
//
// ARCHITECTURE (high-level):
// “Connect” in pq-ssh is intentionally two parallel tracks:
//
//   1) Interactive shell/terminal:
//        - Implemented by launching the system OpenSSH client inside QTermWidget
//        - This provides a robust interactive PTY without re-implementing a full terminal over libssh
//
//   2) Background libssh session:
//        - Used for SFTP features and server-side automation (upload, authorized_keys install)
//        - Connected asynchronously (QtConcurrent) to keep UI responsive
//
// This split is deliberate: OpenSSH is best-in-class for interactive sessions,
// while libssh gives us programmatic control for file operations and workflows.
//

void MainWindow::onProfileSelectionChanged(int /*row*/)
{
    ensureProfileItemSelected();

    const int idx = currentProfileIndex();
    if (idx < 0 || idx >= m_profiles.size())
        return;

    const SshProfile &p = m_profiles[idx];

    if (m_hostField) {
        QString shown = QString("%1@%2").arg(p.user, p.host);
        if (p.port > 0 && p.port != 22)
            shown += QString(":%1").arg(p.port);
        m_hostField->setText(shown);
    }

    if (m_pqDebugCheck)
        m_pqDebugCheck->setChecked(p.pqDebug);
}

void MainWindow::onConnectClicked()
{
    // ARCHITECTURE:
    // Every connect attempt gets a new session id so log lines can be correlated.
    // This is extremely helpful when multiple terminals/windows exist.
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QString rawHost = m_hostField ? m_hostField->text().trimmed() : QString();
    ensureProfileItemSelected();
    const int row = currentProfileIndex();

    // File log gets more context than the UI (UI is intentionally concise).
    logSessionInfo(QString("Connect clicked; profileRow=%1 rawHost='%2'")
                   .arg(row).arg(rawHost));

    // UI validation: ensure a profile is selected.
    if (row < 0 || row >= m_profiles.size()) {
        if (m_statusLabel) m_statusLabel->setText("No profile selected.");
        uiWarn("[WARN] No profile selected.");
        return;
    }

    // Copy profile by value so changes in the editor don’t affect an in-flight connect.
    const SshProfile p = m_profiles[row];

    // UI validation: basic required fields.
    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText("Profile has empty user/host.");
        uiWarn("[WARN] Profile has empty user/host.");
        return;
    }

    const int port = (p.port > 0) ? p.port : 22;
    const QString shownTarget = QString("%1@%2").arg(p.user, p.host);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Connecting to %1 ...").arg(shownTarget));

    // UI: one compact connect line.
    // File log: full profile details for debugging.
    uiInfo(QString("[CONNECT] %1 → %2:%3").arg(p.name, shownTarget).arg(port));

    logSessionInfo(QString("Using profile='%1' user='%2' host='%3' port=%4 keyType='%5' scheme='%6'")
                   .arg(p.name, p.user, p.host)
                   .arg(port)
                   .arg(p.keyType)
                   .arg(p.termColorScheme));

    // UX: sessions can open in their own window or in the shared tabbed window.
    const bool newWindow =
        (m_openInNewWindowCheck && m_openInNewWindowCheck->isChecked());

    // TRACK 1: Start interactive SSH terminal immediately (OpenSSH via QTermWidget).
    // This keeps perceived latency low: user gets a terminal right away.
    openShellForProfile(p, shownTarget, newWindow);

    // PQ status indicator starts in “checking…” and is updated by the probe below.
    updatePqStatusLabel("PQ: checking…", "#888");

    // ----------------------------
    // PQ KEX probe (OpenSSH)
    // ----------------------------
    // ARCHITECTURE:
    // We probe PQ/hybrid KEX support using OpenSSH itself because:
    //   - it reflects the *actual* KEX negotiation OpenSSH would do
    //   - it’s fast, non-invasive, and does not require authentication
    //
    // We run this probe asynchronously using QProcess; it reports via stderr
    // whether the algorithm is supported (or “no matching key exchange”).
    auto *pqProc = new QProcess(this);

    QStringList pqArgs;
    pqArgs << "-o" << "KexAlgorithms=sntrup761x25519-sha512@openssh.com";
    pqArgs << "-o" << "PreferredAuthentications=none";
    pqArgs << "-o" << "PasswordAuthentication=no";
    pqArgs << "-o" << "BatchMode=yes";
    pqArgs << "-o" << "NumberOfPasswordPrompts=0";
    pqArgs << "-o" << "ConnectTimeout=3";
    pqArgs << "-o" << "ConnectionAttempts=1";

    if (port != 22) {
        pqArgs << "-p" << QString::number(port);
    }

    // `true` is a harmless remote command; auth is disabled so we never get that far.
    pqArgs << shownTarget << "true";

    // PQ debug gating: command line is noisy, so UI shows it only in verbose mode.
    // File log always receives it.
    uiDebug(QString("[PQ-PROBE] ssh %1").arg(pqArgs.join(" ")));

    connect(pqProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, pqProc](int exitCode, QProcess::ExitStatus st) {

                // Most of the useful signal is in stderr for ssh negotiation failures.
                const QString err = QString::fromUtf8(pqProc->readAllStandardError());

                logSessionInfo(QString("PQ probe finished exitCode=%1 status=%2 stderrLen=%3")
                               .arg(exitCode)
                               .arg(st == QProcess::NormalExit ? "normal" : "crash")
                               .arg(err.size()));
                uiDebug(QString("[PQ-PROBE] stderr: %1").arg(err.trimmed()));

                // Heuristic: if ssh reports “no matching key exchange”, server didn’t accept the PQ KEX.
                // If it fails for other reasons, we still treat KEX as “supported” unless it explicitly says otherwise.
                const bool pqOk =
                    !(err.contains("no matching key exchange", Qt::CaseInsensitive) ||
                      err.contains("no matching key exchange method", Qt::CaseInsensitive));

                // UI-only indicator: does not change security behavior, just informs the user.
                QSettings s;
                const QString themeId = s.value("ui/theme", "cpunk-dark").toString();

                updatePqStatusLabel(
                    pqOk ? "PQ support: YES" : "PQ support: NO",
                    pqOk ? AppTheme::accent(themeId).name()
                         : QString("#ff5252")
                );

                pqProc->deleteLater();
            });

    pqProc->start("ssh", pqArgs);

    // ----------------------------
    // libssh connect (SFTP support)
    // ----------------------------
    // ARCHITECTURE:
    // libssh session is used for:
    //   - uploadBytes()
    //   - remotePwd()
    //   - installAuthorizedKey()
    //
    // It is explicitly *not* used for the interactive terminal, to avoid re-implementing
    // a robust terminal+PTY stack.
    const QString keyType =
        p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();

    // Current constraint: only “auto/openssh” key types are supported for libssh workflows.
    // If profile says something else, we disable SFTP/workflows but still allow terminal.
    if (!(keyType == "auto" || keyType == "openssh")) {
        uiWarn(QString("[SFTP] Disabled (key_type='%1' not supported yet)").arg(keyType));
        logSessionInfo(QString("SFTP disabled due to unsupported key_type='%1'").arg(keyType));
        if (m_filesTab) m_filesTab->onSshDisconnected();
    } else {
        // Async connect: do not block UI thread.
        // QFutureWatcher result is delivered back on UI thread.
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);

        connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished,
                this, [this, watcher, p, port]() {

                    const auto res = watcher->result();
                    const bool ok = res.first;
                    const QString err = res.second;

                    if (ok) {
                        uiInfo(QString("[SFTP] Ready (%1@%2:%3)").arg(p.user, p.host).arg(port));
                        logSessionInfo("libssh connected OK (SFTP ready)");

                        if (m_filesTab) {
                            // Now remote listing/upload/download can work.
                            m_filesTab->onSshConnected();
                        }
                    } else {
                        // Non-fatal: terminal still works, but uploads/key install will be unavailable.
                        uiWarn(QString("[SFTP] Disabled (libssh connect failed: %1)").arg(err));
                        logSessionInfo(QString("libssh connect FAILED: %1").arg(err));

                        if (m_filesTab) {
                            m_filesTab->onSshDisconnected();
                        }
                    }


                    watcher->deleteLater();
                });

        watcher->setFuture(QtConcurrent::run([this, p]() -> QPair<bool, QString> {
            QString e;
            const bool ok = m_ssh.connectProfile(p, &e);
            return qMakePair(ok, e);
        }));
    }

    // UI state changes: we treat “connect clicked” as entering a connected state.
    // Note: terminal + probe + libssh connect can still fail independently; those failures are reported separately.
    if (m_connectBtn)    m_connectBtn->setEnabled(false);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(true);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Connected: %1").arg(shownTarget));
}


void MainWindow::onProfileDoubleClicked()
{
    ensureProfileItemSelected();
    if (currentProfileIndex() < 0) return;
    onConnectClicked();
}

void MainWindow::onDisconnectClicked()
{
    // ARCHITECTURE:
    // Disconnect currently focuses on the libssh session (SFTP/workflows).
    // The interactive terminal lifecycle is largely driven by the OpenSSH process:
    // it will exit when the user closes the terminal or ssh ends.
    logSessionInfo("Disconnect clicked (user requested)");
    m_ssh.disconnect();
    if (m_filesTab) m_filesTab->onSshDisconnected();
    if (m_connectBtn)    m_connectBtn->setEnabled(true);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(false);

    if (m_statusLabel) m_statusLabel->setText("Disconnected.");
}

void MainWindow::onSendInput()
{
    // ARCHITECTURE:
    // Placeholder input bar. Currently not wired into QTermWidget or libssh.
    // Future options:
    //   - send a command over SshShellWorker/libssh (non-interactive)
    //   - paste into the active terminal tab (interactive)
    const QString text = m_inputField ? m_inputField->text().trimmed() : QString();
    if (text.isEmpty()) return;

    appendTerminalLine(QString("> %1").arg(text));
    appendTerminalLine("[INFO] Shell not implemented yet; input not sent anywhere.");

    if (m_inputField) m_inputField->clear();
}

// ========================
// Drag-drop upload
// ========================
//
// ARCHITECTURE:
// Drag/drop is a terminal UX feature, but upload is a programmatic action,
// so the terminal emits the event and MainWindow delegates to SshClient.
//
// Behavior:
//   - If libssh is not connected, we fall back to a safe local save location.
//     This prevents data loss when the user expects something to happen.
//   - If libssh is connected, we upload to remote $PWD/filename.
//

void MainWindow::onFileDropped(const QString &path, const QByteArray &data)
{
    QFileInfo info(path);
    const QString fileName = info.fileName().isEmpty()
                                 ? QStringLiteral("dropped_file")
                                 : info.fileName();

    appendTerminalLine(QString("[DROP] %1 (%2 bytes)").arg(fileName).arg(data.size()));

    // No libssh session -> save locally to a predictable folder.
    // This is “best effort” behavior and avoids silent failure.
    if (!m_ssh.isConnected()) {
        QDir baseDir(QDir::homePath() + "/pqssh_drops");
        if (!baseDir.exists()) baseDir.mkpath(".");
        const QString outPath = baseDir.filePath(fileName);

        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            appendTerminalLine(QString("[DROP] ERROR: %1").arg(out.errorString()));
            return;
        }
        out.write(data);
        out.close();

        appendTerminalLine(QString("[DROP] Saved locally: %1").arg(outPath));
        return;
    }

    // With libssh session: upload to remote current directory (pwd + filename).
    // Note: remotePwd() is a convenience helper on top of libssh.
    QString err;
    const QString pwd = m_ssh.remotePwd(&err);
    if (pwd.isEmpty()) {
        appendTerminalLine("[UPLOAD] Could not read remote pwd. " + err);
        return;
    }

    const QString remotePath = pwd + "/" + fileName;
    if (!m_ssh.uploadBytes(remotePath, data, &err)) {
        appendTerminalLine("[UPLOAD] FAILED: " + err);
        return;
    }

    appendTerminalLine(QString("[UPLOAD] OK → %1").arg(remotePath));
}

// ========================
// Download selection
// ========================
//
// Placeholder for future: requires a “selection source” (active terminal selection or remote file picker).
void MainWindow::downloadSelectionTriggered()
{
    appendTerminalLine("[DOWNLOAD] Shell not implemented yet (no selection source).");
}

// ========================
// PQ probe
// ========================
//
// ARCHITECTURE:
// This synchronous helper is useful for unit-like checks or alternative flows,
// but the main UI uses an async QProcess version to avoid blocking.
bool MainWindow::probePqSupport(const QString &target)
{
    QProcess proc;
    QStringList args;

    args << "-o" << "KexAlgorithms=sntrup761x25519-sha512@openssh.com";
    args << "-o" << "PreferredAuthentications=none";
    args << "-o" << "PasswordAuthentication=no";
    args << "-o" << "BatchMode=yes";
    args << "-o" << "NumberOfPasswordPrompts=0";
    args << "-o" << "ConnectTimeout=5";
    args << target << "true";

    proc.start("ssh", args);
    if (!proc.waitForFinished(7000)) {
        proc.kill();
        return false;
    }

    const QString err = QString::fromUtf8(proc.readAllStandardError());
    if (err.contains("no matching key exchange method", Qt::CaseInsensitive) ||
        err.contains("no matching key exchange", Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}

// ========================
// Terminal creation
// ========================
//
// ARCHITECTURE:
// createTerm() is responsible for:
//   - instantiating a CpunkTermWidget
//   - applying visual profile settings (color scheme, font, opacity)
//   - launching OpenSSH via /bin/bash -lc "exec ssh ..."
//   - wiring drag/drop and lifecycle hooks
//
// Important security note:
//   - "exec ssh ..." prevents the terminal from staying in a local shell if ssh fails.
//     Without exec, a failed ssh could drop the user into a local shell unexpectedly.
//

CpunkTermWidget* MainWindow::createTerm(const SshProfile &p, QWidget *parent)
{
    auto *term = new CpunkTermWidget(0, parent);
    term->setHistorySize(qMax(0, p.historyLines));

    // Apply terminal rendering preferences first to avoid flicker.
    applyProfileToTerm(term, p);

    QStringList sshArgs;
    sshArgs << "-tt";                 // force TTY (interactive)
    if (p.pqDebug) sshArgs << "-vv";  // verbose OpenSSH only when debugging

    // Add PQ/hybrid KEX to the allowed set.
    sshArgs << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    sshArgs << "-o" << "ConnectTimeout=5";
    sshArgs << "-o" << "ConnectionAttempts=1";

    // Host key policy: accept-new reduces friction while still pinning after first connect.
    sshArgs << "-o" << "StrictHostKeyChecking=accept-new";
    sshArgs << "-o" << ("UserKnownHostsFile=" + QDir::homePath() + "/.ssh/known_hosts");

    const QString kt = p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();
    const bool hasKeyFile = !p.keyFile.trimmed().isEmpty();

    if (kt == "auto" || kt == "openssh") {
        // Standard OpenSSH auth stack: try keys first, then password/KI.
        sshArgs << "-o" << "PubkeyAuthentication=yes";
        sshArgs << "-o" << "IdentitiesOnly=yes";
        sshArgs << "-o" << "PreferredAuthentications=publickey,password,keyboard-interactive";
        sshArgs << "-o" << "PasswordAuthentication=yes";
        sshArgs << "-o" << "KbdInteractiveAuthentication=yes";
        sshArgs << "-o" << "NumberOfPasswordPrompts=3";
        sshArgs << "-o" << "GSSAPIAuthentication=no";
        sshArgs << "-o" << "HostbasedAuthentication=no";

        // Avoid ControlMaster/ControlPersist surprises; every tab is independent.
        sshArgs << "-o" << "ControlMaster=no";
        sshArgs << "-o" << "ControlPath=none";
        sshArgs << "-o" << "ControlPersist=no";

        if (hasKeyFile)
            sshArgs << "-i" << p.keyFile.trimmed();
    } else {
        // Non-standard key types: for now we explicitly fall back to password-based auth.
        appendTerminalLine(QString("[SSH] key_type='%1' not implemented yet → falling back to password auth.")
                           .arg(kt));

        sshArgs << "-o" << "PreferredAuthentications=password,keyboard-interactive";
        sshArgs << "-o" << "PubkeyAuthentication=no";
        sshArgs << "-o" << "IdentityAgent=none";
        sshArgs << "-o" << "KbdInteractiveAuthentication=yes";
        sshArgs << "-o" << "PasswordAuthentication=yes";
        sshArgs << "-o" << "NumberOfPasswordPrompts=3";
        sshArgs << "-o" << "GSSAPIAuthentication=no";
        sshArgs << "-o" << "HostbasedAuthentication=no";
    }

    if (p.port > 0 && p.port != 22)
        sshArgs << "-p" << QString::number(p.port);

    const QString target = (p.user + "@" + p.host);
    sshArgs << target;

    // Debug: show how OpenSSH is invoked (hidden from UI unless PQ debug enabled by appendTerminalLine()).
    appendTerminalLine(QString("[SSH-CMD] ssh %1").arg(sshArgs.join(" ")));

    // Shell quoting helper to safely embed args into bash -lc.
    auto shQuote = [](const QString &s) -> QString {
        QString out = s;
        out.replace("'", "'\"'\"'");
        return "'" + out + "'";
    };

    // SECURITY: use "exec ssh" so a failure cannot leave the user in a local shell.
    QString cmd = "exec ssh";
    for (const QString &a : sshArgs)
        cmd += " " + shQuote(a);

    appendTerminalLine(QString("[SSH-WRAP] %1").arg(cmd));
    appendTerminalLine("[SECURITY] Local shell fallback disabled (exec ssh)");

    // qtermwidget runs the requested program; we use bash -lc to preserve quoting and environment.
    term->setShellProgram("/bin/bash");
    term->setArgs(QStringList() << "-lc" << cmd);

    term->setAutoClose(true);
    term->startShellProgram();

    // Defensive styling: ensure global AppTheme doesn't bleed into terminal internals.
    QTimer::singleShot(0,  term, [term]() { protectTermFromAppStyles(term); });
    QTimer::singleShot(50, term, [term]() { protectTermFromAppStyles(term); });

    // WhiteOnBlack is a special case because Qt palette sometimes starts gray.
    const QString scheme = p.termColorScheme.isEmpty() ? "WhiteOnBlack" : p.termColorScheme;
    if (scheme == "WhiteOnBlack") {
        QTimer::singleShot(0,  term, [term]() { forceBlackBackground(term); });
        QTimer::singleShot(50, term, [term]() { forceBlackBackground(term); });
    }

    // Terminal emits fileDropped; MainWindow decides whether to upload or save locally.
    connect(term, &CpunkTermWidget::fileDropped,
            this, &MainWindow::onFileDropped);

    // Lifecycle: when ssh ends, close the tab/window and disconnect libssh session.
    // This keeps “session state” aligned with actual terminal state.
    connect(term, &QTermWidget::finished, this, [this, term]() {
        appendTerminalLine("[TERM] ssh ended; closing terminal tab/window and disconnecting.");

        if (m_tabWidget) {
            const int idx = m_tabWidget->indexOf(term);
            if (idx >= 0) {
                m_tabWidget->removeTab(idx);
                term->deleteLater();

                if (m_tabWidget->count() == 0 && m_tabbedShellWindow) {
                    m_tabbedShellWindow->close();
                }
            }
        } else {
            QWidget *w = term->window();
            if (w && w != this)
                w->close();
            else
                term->deleteLater();
        }

        onDisconnectClicked();
    });

    appendTerminalLine("[TERM] ssh started (wrapped); terminal will close when ssh exits.");
    return term;
}


// -----------------------------------------------------
// Terminal hardening: force true black background
// -----------------------------------------------------
//
// ARCHITECTURE / WHY THIS EXISTS:
// QTermWidget + Qt palettes + global app styles can lead to a “gray until you type”
// or “theme bleed-through” problem (especially with dark app stylesheets).
//
// This helper is a *pragmatic override* for the common "WhiteOnBlack" expectation:
// - enforce a black base on the terminal widget and all child widgets
// - override both palette + stylesheet (belt & suspenders)
//
// Downsides (acceptable for now):
// - it is heavy-handed and may override fine-grained themes
// - it assumes dark-on-light isn't desired when called
//
static void forceBlackBackground(CpunkTermWidget *term)
{
    if (!term) return;

    // Force paint background using widget palette/stylesheet
    term->setAutoFillBackground(true);

    // Simple direct CSS override (works even when palette doesn’t)
    term->setStyleSheet("background: #000; color: #fff;");

    // Palette override: ensures the “base” used by some Qt paint paths is black.
    QPalette pal = term->palette();
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::Base,   Qt::black);
    pal.setColor(QPalette::Text,   Qt::white);
    term->setPalette(pal);

    // QTermWidget has internal child widgets; global styles can affect them.
    // Apply the same black palette + CSS to every child.
    const auto kids = term->findChildren<QWidget*>();
    for (QWidget *w : kids) {
        w->setAutoFillBackground(true);
        w->setStyleSheet("background: #000; color: #fff;");

        QPalette p2 = w->palette();
        p2.setColor(QPalette::Window, Qt::black);
        p2.setColor(QPalette::Base,   Qt::black);
        p2.setColor(QPalette::Text,   Qt::white);
        w->setPalette(p2);
    }

    // Trigger repaint; useful when the first frames were drawn with the wrong color.
    term->update();
}

// -----------------------------------------------------
// Apply profile settings to terminal instance
// -----------------------------------------------------
//
// ARCHITECTURE:
// This function is the "terminal configuration boundary" between ProfileStore data
// and the live terminal widget.
// It is called by createTerm() before the OpenSSH process is started, so the user
// sees the correct theme/font immediately.
//
// Responsibilities:
// - choose the effective scheme
// - apply scheme/font/opacity
// - guard against global application styles breaking terminal colors
// - sync the surrounding container background so there are no "gutters/borders"
//
void MainWindow::applyProfileToTerm(CpunkTermWidget *term, const SshProfile &p)
{
    if (!term) return;

    // Decide effective scheme: default is WhiteOnBlack.
    const QString scheme =
        p.termColorScheme.isEmpty() ? "WhiteOnBlack" : p.termColorScheme;

    // Note: these are intentionally noisy; appendTerminalLine() hides them unless PQ debug is enabled.
    appendTerminalLine(QString("[TERM] profile=%1 scheme='%2' (raw='%3')")
                       .arg(p.name, scheme, p.termColorScheme));
    appendTerminalLine(QString("[TERM] available schemes: %1")
                       .arg(term->availableColorSchemes().join(", ")));

    // Primary theme selection via QTermWidget scheme names.
    term->setColorScheme(scheme);

    // Prevent AppTheme::dark() global stylesheet from leaking into QTermWidget internals.
    // (See protectTermFromAppStyles below.)
    protectTermFromAppStyles(term);

    // Profile -> terminal font mapping.
    // (We use Monospace + TypeWriter hint to prefer a fixed-width font on each platform.)
    QFont f("Monospace");
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(p.termFontSize > 0 ? p.termFontSize : 11);
    term->setTerminalFont(f);

    // Opacity is a "presentation concern". Keeping it at 1.0 for clarity for now.
    term->setTerminalOpacity(1.0);

    // Special-case hardening:
    // WhiteOnBlack is the most common scheme and the one most likely to suffer “gray start”.
    // We force black explicitly, then re-apply style shielding.
    if (scheme == "WhiteOnBlack") {
        forceBlackBackground(term);
        protectTermFromAppStyles(term);
    }

    // Aesthetic integration:
    // sync terminal container backgrounds to match terminal background so the right-side pane
    // looks like one continuous terminal surface (no visible margins or mismatched containers).
    syncTerminalSurroundingsToTerm(term);
}


// -----------------------------------------------------
// Window/tab management for interactive terminals
// -----------------------------------------------------
//
// ARCHITECTURE:
// MainWindow owns the *policy*:
//   - open each connection in its own QMainWindow (newWindow=true), OR
//   - reuse a shared "Tabs" window with a QTabWidget
//
// createTerm() owns the *terminal instance*.
// openShellForProfile() owns the *hosting surface* (window/tab), focus, and cleanup wiring.
//
void MainWindow::openShellForProfile(const SshProfile &p, const QString &target, bool newWindow)
{
    // target currently unused because we always use p.user/p.host/p.port for actual connection.
    // Keeping parameter allows future call sites to pass custom targets.
    Q_UNUSED(target);

    const int port = (p.port > 0) ? p.port : 22;

    // Used for window/tab titles only (UX), not for the actual ssh cmd construction.
    const QString connLabel = (port != 22)
        ? QString("%1@%2:%3").arg(p.user, p.host).arg(port)
        : QString("%1@%2").arg(p.user, p.host);

    const QString windowTitle = QString("PQ-SSH: %1 (%2)").arg(p.name, connLabel);
    const QString tabTitle    = QString("%1 (%2)").arg(p.name, connLabel);

    if (newWindow) {
        // Mode A: each connection gets its own top-level window.
        auto *w = new QMainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose, true);
        w->setWindowTitle(windowTitle);
        w->resize(p.termWidth > 0 ? p.termWidth : 900,
                  p.termHeight > 0 ? p.termHeight : 500);

        // Terminal is created with this window as parent so Qt will manage widget lifetime.
        auto *term = createTerm(p, w);
        w->setCentralWidget(term);

        // ✅ Install per-profile macro hotkey scoped to THIS window
        installHotkeyMacro(term, w, p);

        // LIFECYCLE NOTE:
        // When the window is destroyed, we also disconnect libssh.
        connect(w, &QObject::destroyed, this, [this]() {
            onDisconnectClicked();
        });

        // Focus management: terminals should accept typing immediately after connect.
        w->show();
        w->raise();
        w->activateWindow();
        focusTerminalWindow(w, term);
        return;
    }

    // Mode B: shared tabs window.
    if (!m_tabbedShellWindow) {
        m_tabbedShellWindow = new QMainWindow();
        m_tabbedShellWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_tabbedShellWindow->setWindowTitle("PQ-SSH Tabs");

        m_tabWidget = new QTabWidget(m_tabbedShellWindow);
        m_tabWidget->setTabsClosable(true);
        m_tabbedShellWindow->setCentralWidget(m_tabWidget);

        connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, [this](int idx) {
            if (!m_tabWidget) return;

            QWidget *w = m_tabWidget->widget(idx);
            m_tabWidget->removeTab(idx);
            if (w) w->deleteLater();

            if (m_tabWidget->count() == 0) {
                if (m_tabbedShellWindow)
                    m_tabbedShellWindow->close();
                onDisconnectClicked();
            }
        });

        connect(m_tabbedShellWindow, &QObject::destroyed, this, [this]() {
            m_tabbedShellWindow = nullptr;
            m_tabWidget = nullptr;
            onDisconnectClicked();
        });

        m_tabbedShellWindow->resize(1000, 650);
    }

    // Create a new terminal tab and activate it.
    auto *term = createTerm(p, m_tabWidget);
    const int idx = m_tabWidget->addTab(term, tabTitle);
    m_tabWidget->setCurrentIndex(idx);

    // ✅ Install per-profile macro hotkey scoped to the TABBED window
    installHotkeyMacro(term, m_tabbedShellWindow, p);

    // Give the tabbed window a contextual title so user sees which host is “current”.
    m_tabbedShellWindow->setWindowTitle(QString("PQ-SSH Tabs — %1").arg(connLabel));

    // Show + focus so typing works immediately.
    m_tabbedShellWindow->show();
    m_tabbedShellWindow->raise();
    m_tabbedShellWindow->activateWindow();
    focusTerminalWindow(m_tabbedShellWindow, term);
}


// -----------------------------------------------------
// Style shielding for terminal widget
// -----------------------------------------------------
//
// ARCHITECTURE / WHY THIS EXISTS:
// AppTheme::dark() applies a global stylesheet. QTermWidget contains internal widgets
// that can inherit those styles and render with wrong colors/margins.
//
// This helper tries to "isolate" terminal rendering by forcing children to use palette()
// values (Base/Text) instead of inheriting app-level QWidget rules.
//
static void protectTermFromAppStyles(CpunkTermWidget *term)
{
    if (!term) return;

    // Shield rule uses palette(Base/Text) to keep QTermWidget in control of its own scheme.
    const QString shield =
        "QWidget { background-color: palette(Base); color: palette(Text); }";

    term->setAutoFillBackground(true);
    term->setStyleSheet(shield);

    // Apply to internal widgets too.
    const auto kids = term->findChildren<QWidget*>();
    for (QWidget *w : kids) {
        w->setAutoFillBackground(true);
        w->setStyleSheet(shield);
    }
}


// -----------------------------------------------------
// Help → Open log file
// -----------------------------------------------------
//
// ARCHITECTURE:
// Logger is installed early (main.cpp) so qInfo/qWarning output goes to a stable log file.
// This action is the “supportability hook”: users/devs can jump straight to the log without
// hunting paths or opening ~/.pq-ssh manually.
//
// Design choices:
// - We treat “no log path” as non-fatal and just warn.
// - We open via QDesktopServices so the OS chooses the default viewer/editor.
//
void MainWindow::onOpenLogFile()
{
    const QString path = Logger::logFilePath();

    // Guard: Logger might be disabled/misconfigured, or path resolution failed.
    if (path.isEmpty()) {
        // UI-visible message (subject to appendTerminalLine filters) + real qWarning for file.
        appendTerminalLine("[LOG] Log file path not available.");
        qWarning() << "Log file path is empty";
        return;
    }

    // Let OS decide how to open (text editor, file viewer, etc.)
    const QUrl url = QUrl::fromLocalFile(path);
    QDesktopServices::openUrl(url);

    // Always log the action for traceability.
    qInfo() << "Opened log file:" << path;
}


// -----------------------------------------------------
// Session-tagged logging helper
// -----------------------------------------------------
//
// ARCHITECTURE:
// A connection attempt gets a session ID (m_sessionId) so logs from multiple attempts
// can be grouped together, especially when UI actions + async jobs interleave.
//
// This function writes *only to the log file* (via qInfo), not the UI terminal.
//
void MainWindow::logSessionInfo(const QString& msg)
{
    const QString sid = m_sessionId.isEmpty() ? "-" : m_sessionId;
    qInfo().noquote() << QString("[SESSION %1] %2").arg(sid, msg);
}


// -----------------------------------------------------
// UI logging helpers (UI + file log)
// -----------------------------------------------------
//
// ARCHITECTURE:
// There are two audiences for messages:
//  1) user-facing UI terminal (clean + minimal in normal mode)
//  2) developer/support log file (always detailed)
//
// uiInfo/uiWarn always echo to UI + log.
// uiDebug always goes to log; it goes to UI only if PQ debug is enabled.
//
void MainWindow::uiInfo(const QString& msg)
{
    appendTerminalLine(msg);
    qInfo().noquote() << "[UI]" << msg;
}

void MainWindow::uiWarn(const QString& msg)
{
    appendTerminalLine(msg);
    qWarning().noquote() << "[UI]" << msg;
}

void MainWindow::uiDebug(const QString& msg)
{
    // PQ debug acts as the "verbosity gate" for the on-screen terminal.
    // Even when off, debug messages still go to the file log.
    const bool verbose = (m_pqDebugCheck && m_pqDebugCheck->isChecked());
    qInfo().noquote() << "[UI-DEBUG]" << msg;          // always goes to file
    if (verbose) appendTerminalLine(msg);              // UI only if enabled
}

// Convenience accessor so other code can check verbosity without duplicating checkbox logic.
bool MainWindow::uiVerbose() const
{
    return (m_pqDebugCheck && m_pqDebugCheck->isChecked());
}


// -----------------------------------------------------
// Help → User Manual (embedded resource)
// -----------------------------------------------------
//
// ARCHITECTURE:
// The manual is shipped inside the binary via Qt resources (qrc:/docs/...).
// This avoids “missing manual” issues in packaging and ensures offline availability.
//
// We render it in a QTextBrowser inside a QDialog:
// - fast to implement
// - good enough for a help page (links, anchors, basic CSS)
// - opens external links in system browser
//
// NOTE: defaultStyleSheet acts as a fallback for Qt’s HTML renderer so the manual
// stays consistent with the dark CPUNK theme even if the embedded CSS doesn't fully apply.
//
void MainWindow::onOpenUserManual()
{
    const QUrl url("qrc:/docs/user-manual.html");

    // Guard against packaging/qrc mistakes.
    if (!QFile::exists(":/docs/user-manual.html")) {
        appendTerminalLine("[WARN] User manual resource missing: :/docs/user-manual.html");
        return;
    }

    // Use a dialog so it behaves like “Help” and doesn’t replace the main app window.
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowTitle("PQ-SSH User Manual");
    dlg->resize(980, 720);

    auto *layout = new QVBoxLayout(dlg);

    auto *browser = new QTextBrowser(dlg);

    // Fallback styling for Qt HTML engine (prevents blue links / bright background)
    // NOTE: There is an extra "}" in the original string block (see below).
    // It doesn’t always break rendering, but you may want to clean it up.
    browser->document()->setDefaultStyleSheet(
        "body { background:#0b0b0c; color:#e7e7ea; }"
        "a, a:link, a:visited { color:#00ff99; text-decoration:none; }"
        "a:hover { text-decoration:underline; color:#7cffc8; }"
        ".nav a, a.btn {"
        "  display:inline-block;"
        "  padding:6px 12px;"
        "  border-radius:999px;"
        "  background:rgba(0,255,153,.08);"
        "  border:1px solid rgba(0,255,153,.28);"
        "  text-decoration:none;"
        "}"

    );

    // Let user click docs links and open them externally.
    browser->setOpenExternalLinks(true);

    // Load HTML from qrc (offline, packaged)
    browser->setSource(url);

    layout->addWidget(browser);

    dlg->setLayout(layout);
    dlg->show();
}


// -----------------------------------------------------
// DEV tool: validate Dilithium key decryption/unlock
// -----------------------------------------------------
//
// ARCHITECTURE / INTENT:
// This is a developer-facing sanity check for the encrypted Dilithium key format.
// It is intentionally gated behind the PQ debug checkbox so end users don’t stumble
// into a scary crypto flow.
//
// Safety properties in this implementation:
// - It never prints plaintext key material.
// - It logs only sizes + SHA-256 of plaintext (digest is safe for comparisons).
// - It wipes decrypted plaintext with sodium_memzero() ASAP.
//
// Flow:
//  1) choose .enc file
//  2) validate minimal format before prompting for passphrase
//  3) prompt passphrase (dev-only)
//  4) decrypt using decryptDilithiumKey()
//  5) validate basic expectations, show OK/FAIL
//
void MainWindow::onTestUnlockDilithiumKey()
{
    // Hard gate: prevents accidental exposure of this dev-only path.
    if (!(m_pqDebugCheck && m_pqDebugCheck->isChecked())) {
        appendTerminalLine("[DEV] PQ debug is OFF. Enable it to use the tester.");
        return;
    }

    const QString startDir = QDir(QDir::homePath()).filePath(".pq-ssh/keys");
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Select encrypted Dilithium private key",
        startDir,
        "Encrypted keys (*.enc);;All files (*)"
    );
    if (path.isEmpty())
        return;

    // Read entire encrypted blob.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "DEV test failed", "Cannot read file:\n" + f.errorString());
        return;
    }
    const QByteArray enc = f.readAll();
    f.close();

    // Format guard BEFORE passphrase prompt:
    // avoids asking user for a passphrase for a file that’s obviously invalid/truncated.
    //
    // Expected: MAGIC(6) + SALT(16) + NONCE(24) + CIPHERTEXT(tag...)
    if (enc.size() < (6 + crypto_pwhash_SALTBYTES + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + 16)) {
        QMessageBox::warning(this, "DEV test failed",
                             "File is too small to be a valid PQSSH encrypted key.\n"
                             "Expected: MAGIC(6)+SALT(16)+NONCE(24)+CIPHERTEXT(tag...).");
        return;
    }

    // Lightweight metadata logs (safe).
    const QByteArray magic = enc.left(6);
    appendTerminalLine(QString("[DEV] Selected: %1").arg(QFileInfo(path).fileName()));
    appendTerminalLine(QString("[DEV] Enc size: %1 bytes").arg(enc.size()));
    appendTerminalLine(QString("[DEV] Magic: '%1'").arg(QString::fromLatin1(magic)));

    // Magic mismatch isn’t fatal for dev testing, but it’s a big clue.
    if (magic != "PQSSH1") {
        appendTerminalLine("[DEV][WARN] Magic mismatch. Expected 'PQSSH1'. Continuing anyway to test decrypt...");
    }

    // Passphrase prompt.
    // NOTE: This currently bypasses the m_ssh passphrase provider and asks directly here.
    // That’s fine for dev mode, but if you want consistency, you can route via SshClient.
    bool ok = false;
    QString pass;
    {
        bool localOk = false;
        pass = QInputDialog::getText(this,
                                     "Dilithium Key Passphrase",
                                     QString("Enter passphrase for:\n%1").arg(QFileInfo(path).fileName()),
                                     QLineEdit::Password,
                                     QString(),
                                     &localOk);
        ok = localOk;
    }

    if (!ok) {
        appendTerminalLine("[DEV] Cancelled by user.");
        return;
    }

    // Decrypt and validate without ever printing the plaintext.
    QByteArray plain;
    QString decErr;
    if (!decryptDilithiumKey(enc, pass, &plain, &decErr)) {
        appendTerminalLine(QString("[DEV][FAIL] Decrypt failed: %1").arg(decErr));
        QMessageBox::warning(this, "DEV test failed", "Decrypt failed:\n" + decErr);
        return;
    }

    if (plain.isEmpty()) {
        appendTerminalLine("[DEV][FAIL] Decrypt returned empty plaintext.");
        QMessageBox::warning(this, "DEV test failed", "Decrypt returned empty plaintext.");
        return;
    }

    // Report only non-sensitive validation signals:
    // - plaintext size
    // - SHA-256 digest for comparing outputs across machines/runs
    const QByteArray digest = QCryptographicHash::hash(plain, QCryptographicHash::Sha256).toHex();
    appendTerminalLine(QString("[DEV][OK] Decrypted plaintext size: %1 bytes").arg(plain.size()));
    appendTerminalLine(QString("[DEV][OK] Plain SHA256: %1").arg(QString::fromLatin1(digest)));

    // Heuristic: Dilithium private key material is typically multiple KB.
    if (plain.size() < 2048) {
        appendTerminalLine("[DEV][WARN] Plaintext is unexpectedly small for Dilithium private key material.");
    }

    // Clear decrypted secret bytes as soon as we’re done.
    sodium_memzero(plain.data(), (size_t)plain.size());

    QMessageBox::information(this, "DEV test OK",
                             "Decrypt OK.\n\n"
                             "Validated format + passphrase unlock.\n"
                             "Details written to the terminal/log.");
}

void MainWindow::applySavedSettings()
{
    QSettings s;

    // Theme
    applyCurrentTheme();

    // Logging level
    Logger::setLogLevel(s.value("logging/level", 1).toInt());

    // Log file override: empty => revert to default path
    const QString logFile = s.value("logging/filePath", "").toString().trimmed();
    Logger::setLogFilePathOverride(logFile);

    // Audit dir override: empty => revert to default dir
    const QString auditDir = s.value("audit/dirPath", "").toString().trimmed();
    AuditLogger::setAuditDirOverride(auditDir);
}


void MainWindow::onOpenSettingsDialog()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // Apply immediately (theme + log level)
    applySavedSettings();
    rebuildProfileList();

    appendTerminalLine("[INFO] Settings updated.");
    if (m_statusLabel) m_statusLabel->setText("Settings updated.");
}

void MainWindow::onOpenSettings()
{
    QMessageBox::information(this, "Settings", "Settings dialog (coming next).");
}

void MainWindow::installHotkeyMacro(CpunkTermWidget* term, QWidget* shortcutScope, const SshProfile& p)
{
    if (!term || !shortcutScope) return;

    // If no new macros exist, fall back to legacy single macro (backward-compat)
    QVector<ProfileMacro> macros = p.macros;
    if (macros.isEmpty()) {
        ProfileMacro m;
        m.name      = "";
        m.shortcut  = p.macroShortcut.trimmed();
        m.command   = p.macroCommand;
        m.sendEnter = p.macroEnter;
        if (!m.shortcut.isEmpty() && !m.command.trimmed().isEmpty())
            macros.push_back(m);
    }

    if (macros.isEmpty())
        return;

    // Protect against term being destroyed while shortcutScope still lives
    QPointer<CpunkTermWidget> termPtr(term);

    for (int i = 0; i < macros.size(); ++i) {
        const ProfileMacro& m = macros[i];

        const QString shortcutText = m.shortcut.trimmed();
        const QString cmd          = m.command; // keep as-is
        const bool sendEnter       = m.sendEnter;

        // Only enable if BOTH shortcut + command are set
        if (shortcutText.isEmpty() || cmd.trimmed().isEmpty())
            continue;

        const QKeySequence seq(shortcutText);
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (seq.isEmpty()) {
            uiWarn(QString("[MACRO] Invalid shortcut '%1' (profile '%2')").arg(shortcutText, p.name));
            continue;
        }
#endif

        // Parent to the scope so it dies with the window/tabs container
        auto* sc = new QShortcut(seq, shortcutScope);
        sc->setContext(Qt::WidgetWithChildrenShortcut);

        const QString shownName = m.name.trimmed().isEmpty()
            ? QString("Macro %1").arg(i + 1)
            : m.name.trimmed();

        uiDebug(QString("[MACRO] Bound %1 → \"%2\" (enter=%3) [%4] profile '%5'")
                    .arg(seq.toString(),
                         cmd.trimmed(),
                         sendEnter ? "yes" : "no",
                         shownName,
                         p.name));

        connect(sc, &QShortcut::activated, this, [this, termPtr, cmd, sendEnter, shownName]() {
            if (!termPtr) return;

            QString out = cmd;
            if (sendEnter)
                out += "\n";

            termPtr->sendText(out);
            uiDebug(QString("[MACRO] Sent (%1): %2").arg(shownName, cmd.trimmed()));
        });
    }
}

void MainWindow::onKexNegotiated(const QString& prettyText, const QString& rawKex)
{
    const bool pq =
        rawKex.contains("mlkem", Qt::CaseInsensitive) ||
        rawKex.contains("sntrup", Qt::CaseInsensitive);

    updatePqStatusLabel(
        pq ? ("PQ KEX: " + prettyText) : ("KEX: " + prettyText),
        pq ? "#00FF99" : "#9AA0A6"
    );

    if (m_pqStatusLabel)
        m_pqStatusLabel->setToolTip("Negotiated KEX: " + rawKex);
}

void MainWindow::onIdentityManagerRequested()
{
    // If already open, bring to front
    if (m_identityDlg) {
        m_identityDlg->raise();
        m_identityDlg->activateWindow();
        return;
    }

    // IMPORTANT: parent = nullptr so it’s freely movable and not “attached”
    m_identityDlg = new IdentityManagerDialog(nullptr);
    m_identityDlg->setAttribute(Qt::WA_DeleteOnClose, true);
    m_identityDlg->setWindowTitle("CPUNK PQ-SSH — Identity Manager");

    // When closed, clear pointer
    connect(m_identityDlg, &QObject::destroyed, this, [this]() {
        m_identityDlg = nullptr;
    });

    m_identityDlg->show();
    m_identityDlg->raise();
    m_identityDlg->activateWindow();
}

void MainWindow::onImportOpenSshConfig()
{
    // If already open, bring to front
    if (m_sshPlanDlg) {
        m_sshPlanDlg->raise();
        m_sshPlanDlg->activateWindow();
        return;
    }

    const QString sshDir = QDir(QDir::homePath()).filePath(".ssh");
    const QString path   = QDir(sshDir).filePath("config");

    QDir().mkpath(sshDir);

    // If config missing, offer to create it
    if (!QFileInfo::exists(path)) {
        const auto ans = QMessageBox::question(
            this,
            "OpenSSH config not found",
            "No OpenSSH config file was found at:\n\n" + path +
            "\n\nCreate a starter ~/.ssh/config now?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes
        );

        if (ans != QMessageBox::Yes)
            return;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QMessageBox::critical(this, "Cannot create config",
                                  "Failed to create:\n" + path + "\n\n" + f.errorString());
            return;
        }

        const QByteArray tpl =
            "# OpenSSH client configuration\n"
            "#\n"
            "# Example:\n"
            "# Host myserver\n"
            "#     HostName 192.168.1.10\n"
            "#     User root\n"
            "#     Port 22\n"
            "#     IdentityFile ~/.ssh/id_ed25519\n"
            "\n";
        f.write(tpl);
        f.close();

        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // Detect "only comments" -> inform user (optional but helpful)
    {
        QFile rf(path);
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(rf.readAll());
            rf.close();

            bool hasHost = false;
            for (const QString& line : text.split('\n')) {
                const QString t = line.trimmed();
                if (t.startsWith("Host ", Qt::CaseInsensitive)) {
                    hasHost = true;
                    break;
                }
            }

            if (!hasHost) {
                QMessageBox::information(
                    this,
                    "Nothing to import yet",
                    "Your ~/.ssh/config contains no active Host entries (only comments).\n\n"
                    "Add a block like:\n\n"
                    "Host myserver\n"
                    "    HostName 192.168.1.10\n"
                    "    User root\n"
                    "    Port 22\n\n"
                    "Then try Import again."
                );
                // You can return here if you want to avoid opening an empty dialog:
                // return;
            }
        }
    }

    // Parse config
    const SshConfigParseResult parsed = SshConfigParser::parseFile(path);

    // Existing profile names for diff
    QStringList existingNames;
    existingNames.reserve(m_profiles.size());
    for (const auto& p : m_profiles)
        existingNames << p.name;

    // Open Import Plan dialog (modeless)
    m_sshPlanDlg = new SshConfigImportPlanDialog(path, parsed, existingNames, nullptr);
    m_sshPlanDlg->setAttribute(Qt::WA_DeleteOnClose, true);
    m_sshPlanDlg->setWindowTitle("CPUNK PQ-SSH — Import Plan");

    connect(m_sshPlanDlg, &QObject::destroyed, this, [this]() {
        m_sshPlanDlg = nullptr;
    });

    connect(m_sshPlanDlg, &SshConfigImportPlanDialog::applyRequested,
            this, &MainWindow::onApplyImportedProfiles);

    m_sshPlanDlg->show();
    m_sshPlanDlg->raise();
    m_sshPlanDlg->activateWindow();
}


void MainWindow::onApplyImportedProfiles(const QVector<ImportedProfile>& creates,
                                        const QVector<ImportedProfile>& updates)
{
    Q_UNUSED(updates); // v1: implement updates later

    int added = 0;

    for (const auto& ip : creates) {
        SshProfile p;
        p.name = ip.name;

        // Adjust these fields to match your SshProfile struct:
        p.host = ip.hostName;
        p.user = ip.user;
        p.port = ip.port;
        // p.identityFile = ip.identityFile; // only if you have it in SshProfile

        m_profiles.push_back(p);
        added++;
    }

    if (added > 0) {
        saveProfilesToDisk();
        rebuildProfileList();
        appendTerminalLine(QString("[INFO] Imported %1 profile(s) from ~/.ssh/config").arg(added));
        if (m_statusLabel) m_statusLabel->setText(QString("Imported %1 profiles").arg(added));
    } else {
        appendTerminalLine("[INFO] Import plan applied: no profiles added.");
        if (m_statusLabel) m_statusLabel->setText("Import plan applied: no changes.");
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    AuditLogger::writeEvent("session.end");
    QMainWindow::closeEvent(e);
}