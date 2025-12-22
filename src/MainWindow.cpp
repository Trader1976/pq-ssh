// MainWindow.cpp
#include "MainWindow.h"
#include "KeyGeneratorDialog.h"
#include "KeyMetadataUtils.h"
#include "FilesTab.h"
#include "IdentityManagerDialog.h"
#include "Audit/AuditLogViewerDialog.h"

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
#include <QTabWidget>
#include <QFont>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QUuid>
#include <QFrame>
#include <QToolButton>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QCloseEvent>
#include <QEvent>
#include <QMetaObject>
#include <QVariant>
#include <QDialogButtonBox>

#include "AppTheme.h"
#include "ProfileStore.h"
#include "ProfilesEditorDialog.h"
#include "SshClient.h"
#include "CpunkTermWidget.h"
#include <qtermwidget5/qtermwidget.h>
#include "Logger.h"
#include "DilithiumKeyCrypto.h"
#include <sodium.h>
#include "SshConfigImportPlanDialog.h"
#include "SshConfigParser.h"
#include "SshConfigImportPlan.h"
#include "SshConfigImportDialog.h"
#include "SettingsDialog.h"
#include "Fleet/FleetWindow.h"
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

class CpunkTermWidget;
static void forceBlackBackground(CpunkTermWidget *term);
static void protectTermFromAppStyles(CpunkTermWidget *term);
static void stopTerm(CpunkTermWidget* term);

// =====================================================
// Helpers for clean command logging
// =====================================================
static QString shellQuote(const QString &s)
{
    if (s.isEmpty())
        return "''";

    QString out = s;
    out.replace('\'', "'\"'\"'");
    return "'" + out + "'";
}

static QString prettyCommandLine(const QString &exe, const QStringList &args)
{
    QStringList parts;
    parts << shellQuote(exe);
    for (const auto &a : args)
        parts << shellQuote(a);
    return parts.join(" ");
}

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
        it->setToolTip(tr("%1@%2:%3  [%4]")
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

bool saveNewAppPassword(const QString& newPass, QString* errOut)
{
    QByteArray pw = newPass.toUtf8();

    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash,
                          pw.constData(),
                          (unsigned long long)pw.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        if (errOut) *errOut = QObject::tr("Out of memory while hashing password.");
        return false;
    }

    QSettings s;
    s.setValue("appLock/hash", QByteArray(hash));
    return true;
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
    qInfo() << "MainWindow constructing";
    qInfo() << "App:" << QCoreApplication::applicationName()
            << "Version:" << QCoreApplication::applicationVersion();
    qInfo() << "Qt:" << QT_VERSION_STR
            << "OS:" << QSysInfo::prettyProductName()
            << "Platform:" << QGuiApplication::platformName();

    // Global widget theme. Terminal colors are handled separately.
    applySavedSettings();

    setWindowTitle(tr("CPUNK PQ-SSH"));
    resize(1200, 700);

    setupUi();
    loadProfiles();      // loadProfiles() calls rebuildProfileList()
    setupMenus();

    // Passphrase prompt provider for libssh when an OpenSSH private key is encrypted.
    m_ssh.setPassphraseProvider([this](const QString& keyFile, bool *ok) -> QString {
        const QString title = tr("SSH Key Passphrase");
        const QString label = keyFile.trimmed().isEmpty()
            ? tr("Enter passphrase for private key:")
            : tr("Enter passphrase for key:\n%1").arg(QFileInfo(keyFile).fileName());

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

    // ---- App lock (optional) ----
    {
        QSettings s;
        const bool lockEnabled = s.value("appLock/enabled", false).toBool();
        const QByteArray hash  = s.value("appLock/hash", "").toByteArray();

        if (lockEnabled) {
            if (hash.isEmpty()) {
                QMessageBox::warning(
                    this,
                    tr("App lock"),
                    tr("App lock is enabled but no password is set.\n"
                       "Disable app lock in Settings or set a password.")
                );
            } else {
                this->hide();

                const bool ok = showStartupUnlockDialog();
                if (!ok) {
                    qApp->quit();
                    return;
                }

                this->show();
                this->raise();
                this->activateWindow();
            }
        }
    }

    // ---- Startup security warning: expired keys ----
    {
        const QString metaPath = QDir(QDir::homePath()).filePath(".pq-ssh/keys/metadata.json");
        qInfo() << "Checking key metadata:" << metaPath;

        QString autoErr;
        autoExpireMetadataFile(metaPath, &autoErr);

        if (!autoErr.isEmpty()) {
            appendTerminalLine(tr("[WARN] %1").arg(autoErr));
            qWarning() << "autoExpireMetadataFile:" << autoErr;
        }

        QString e;
        const int expired = countExpiredKeysInMetadata(&e);

        if (!e.isEmpty()) {
            appendTerminalLine(tr("[WARN] %1").arg(e));
            qWarning() << "countExpiredKeysInMetadata:" << e;
        }

        qInfo() << "Expired keys:" << expired;

        if (expired > 0) {
            const QString msg =
                tr("⚠ WARNING: You have %1 expired SSH key(s). Open Key Generator → Keys tab to review/rotate.")
                    .arg(expired);

            if (m_statusLabel) {
                m_statusLabel->setText(msg);
                m_statusLabel->setStyleSheet("color: #ff5252; font-weight: bold;");
            }

            appendTerminalLine(tr("[SECURITY] %1").arg(msg));
            qWarning() << "SECURITY:" << msg;
        }
    }

    qInfo() << "MainWindow constructed OK";
}

MainWindow::~MainWindow()
{
    m_ssh.disconnect();
}

// ========================
// UI construction
// ========================
void MainWindow::setupUi()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    setCentralWidget(splitter);

    // ============================
    // Left pane: profile list
    // ============================
    auto *profilesWidget = new QWidget(splitter);
    auto *profilesLayout = new QVBoxLayout(profilesWidget);
    profilesLayout->setContentsMargins(8, 8, 8, 8);
    profilesLayout->setSpacing(6);

    auto *profilesLabel = new QLabel(tr("Profiles"), profilesWidget);
    profilesLabel->setStyleSheet("font-weight: bold;");

    m_profileList = new QListWidget(profilesWidget);
    m_profileList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_editProfilesBtn = new QPushButton(tr("Edit profiles…"), profilesWidget);
    m_editProfilesBtn->setToolTip(tr("Manage profiles inside the app"));

    profilesLayout->addWidget(profilesLabel);
    profilesLayout->addWidget(m_profileList, 1);
    profilesLayout->addWidget(m_editProfilesBtn, 0);
    profilesWidget->setLayout(profilesLayout);

    // ============================
    // Right pane: connect controls + terminal + status
    // ============================
    auto *rightWidget = new QWidget(splitter);

    auto *outer = new QVBoxLayout(rightWidget);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    // --- Top bar ---
    auto *topBar = new QWidget(rightWidget);
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);

    auto *hostLabel = new QLabel(tr("Host:"), topBar);
    m_hostField = new QLineEdit(topBar);
    m_hostField->setPlaceholderText(tr("user@hostname"));

    m_connectBtn = new QPushButton(tr("Connect"), topBar);
    m_disconnectBtn = new QPushButton(tr("Disconnect"), topBar);
    m_disconnectBtn->setEnabled(false);

    topLayout->addWidget(hostLabel);
    topLayout->addWidget(m_hostField, 1);
    topLayout->addWidget(m_connectBtn);
    topLayout->addWidget(m_disconnectBtn);
    topBar->setLayout(topLayout);

    // --- Terminal container: QTabWidget with Log + Files ---
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

    m_mainTabs->addTab(logPage, tr("Log"));
    m_mainTabs->addTab(m_filesTab, tr("Files"));

    termLayout->addWidget(m_mainTabs, 1);
    termContainer->setLayout(termLayout);

    // --- Input bar ---
    auto *inputBar = new QWidget(rightWidget);
    auto *inputLayout = new QHBoxLayout(inputBar);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(6);

    m_inputField = new QLineEdit(inputBar);
    m_inputField->setPlaceholderText(tr("Type command (not wired yet) ..."));

    m_sendBtn = new QPushButton(tr("Send"), inputBar);
    m_sendBtn->setEnabled(true);

    inputLayout->addWidget(m_inputField, 1);
    inputLayout->addWidget(m_sendBtn);
    inputBar->setLayout(inputLayout);

    // --- Bottom bar ---
    auto *bottomBar = new QWidget(rightWidget);
    auto *bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);

    m_statusLabel = new QLabel(tr("Ready."), bottomBar);
    m_statusLabel->setStyleSheet("color: gray;");

    m_pqStatusLabel = new QLabel(tr("PQ: unknown"), bottomBar);
    m_pqStatusLabel->setStyleSheet("color: #888; font-weight: bold;");

    m_pqDebugCheck = new QCheckBox(tr("PQ debug"), bottomBar);
    m_pqDebugCheck->setChecked(true);

    m_openInNewWindowCheck = new QCheckBox(tr("Open new connection in NEW window"), bottomBar);
    m_openInNewWindowCheck->setChecked(true);

    bottomLayout->addWidget(m_statusLabel, 1);
    bottomLayout->addWidget(m_openInNewWindowCheck, 0);
    bottomLayout->addWidget(m_pqStatusLabel, 0);
    bottomLayout->addWidget(m_pqDebugCheck, 0);
    bottomBar->setLayout(bottomLayout);

    outer->addWidget(topBar);
    outer->addWidget(termContainer, 1);
    outer->addWidget(inputBar, 0);
    outer->addWidget(bottomBar);

    splitter->addWidget(profilesWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    splitter->setStyleSheet("QSplitter::handle { background-color: #121212; }");

    // Wiring
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

                if (pq) {
                    appendTerminalLine(
                        tr("[PQ] 🧬 Post-Quantum key exchange established → %1")
                            .arg(pretty)
                    );
                } else {
                    appendTerminalLine(
                        tr("[KEX] Classical key exchange negotiated → %1")
                            .arg(pretty)
                    );
                }

                updatePqStatusLabel(
                    pq ? (tr("Post-Quantum KEX: %1").arg(pretty))
                       : (tr("KEX: %1").arg(pretty)),
                    pq ? "#00FF99" : "#9AA0A6"
                );

                if (m_pqStatusLabel)
                    m_pqStatusLabel->setToolTip(tr("Negotiated KEX: %1").arg(raw));
            });
}

void MainWindow::setupMenus()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    QAction *settingsAct = fileMenu->addAction(tr("Settings…"));
    settingsAct->setToolTip(tr("Open PQ-SSH settings"));
    connect(settingsAct, &QAction::triggered, this, [this]() {

        if (m_settingsDlg) {
            m_settingsDlg->raise();
            m_settingsDlg->activateWindow();
            return;
        }

        m_settingsDlg = new SettingsDialog(nullptr);
        m_settingsDlg->setAttribute(Qt::WA_DeleteOnClose, true);
        m_settingsDlg->setWindowTitle(tr("CPUNK PQ-SSH — Settings"));

        connect(m_settingsDlg, &QObject::destroyed, this, [this]() {
            m_settingsDlg = nullptr;
        });

        connect(m_settingsDlg, &QDialog::accepted, this, [this]() {
            applySavedSettings();
            rebuildProfileList();
            appendTerminalLine(tr("[INFO] Settings updated."));
            if (m_statusLabel) m_statusLabel->setText(tr("Settings updated."));
        });

        m_settingsDlg->show();
        m_settingsDlg->raise();
        m_settingsDlg->activateWindow();
    });

    QAction *importSshConfigAct = fileMenu->addAction(tr("Import OpenSSH config…"));
    importSshConfigAct->setToolTip(tr("Read ~/.ssh/config and preview entries (no profiles are created yet)."));
    connect(importSshConfigAct, &QAction::triggered, this, &MainWindow::onImportOpenSshConfig);

    fileMenu->addSeparator();

    QAction *quitAct = fileMenu->addAction(tr("Quit"));
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    // Tools
    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    static QPointer<FleetWindow> g_fleetWin;

    QAction *fleetAct = toolsMenu->addAction(tr("Fleet jobs…"));
    fleetAct->setToolTip(tr("Run the same job across multiple profiles/hosts (e.g., deploy to 10 servers)."));

    connect(fleetAct, &QAction::triggered, this, [this]() {
        if (g_fleetWin) {
            g_fleetWin->raise();
            g_fleetWin->activateWindow();
            return;
        }

        g_fleetWin = new FleetWindow(m_profiles, nullptr);
        g_fleetWin->setAttribute(Qt::WA_DeleteOnClose, true);

        connect(g_fleetWin, &QObject::destroyed, this, [&]() {
            g_fleetWin = nullptr;
        });

        g_fleetWin->show();
        g_fleetWin->raise();
        g_fleetWin->activateWindow();
    });

    // Keys
    auto *keysMenu = menuBar()->addMenu(tr("&Keys"));

    QAction *keyGenAct = new QAction(tr("Key Generator…"), this);
    keyGenAct->setToolTip(tr("Generate keys and optionally install the public key to a server profile"));
    keysMenu->addAction(keyGenAct);

    connect(keyGenAct, &QAction::triggered, this, [this]() {

        if (m_keyGenerator) {
            m_keyGenerator->raise();
            m_keyGenerator->activateWindow();
            return;
        }

        QStringList names;
        names.reserve(m_profiles.size());
        for (const auto &p : m_profiles)
            names << p.name;

        m_keyGenerator = new KeyGeneratorDialog(names, nullptr);
        m_keyGenerator->setAttribute(Qt::WA_DeleteOnClose, true);
        m_keyGenerator->setWindowTitle(tr("CPUNK PQ-SSH — Key Generator"));

        connect(m_keyGenerator, &KeyGeneratorDialog::installPublicKeyRequested,
                this, &MainWindow::onInstallPublicKeyRequested);

        connect(m_keyGenerator, &QObject::destroyed, this, [this]() {
            m_keyGenerator = nullptr;
        });

        m_keyGenerator->show();
        m_keyGenerator->raise();
        m_keyGenerator->activateWindow();
    });

    QAction *installPubAct = new QAction(tr("Install public key to server…"), this);
    installPubAct->setToolTip(tr("Append an OpenSSH public key to ~/.ssh/authorized_keys on the selected profile host."));
    keysMenu->addAction(installPubAct);

    connect(installPubAct, &QAction::triggered, this, [this]() {
        ensureProfileItemSelected();
        const int row = currentProfileIndex();
        if (row < 0 || row >= m_profiles.size()) {
            QMessageBox::warning(this, tr("Key install"), tr("No profile selected."));
            return;
        }

        const QString pubPath = QFileDialog::getOpenFileName(
            this,
            tr("Select OpenSSH public key (.pub)"),
            QDir::homePath() + "/.ssh",
            tr("Public keys (*.pub);;All files (*)")
        );
        if (pubPath.isEmpty()) return;

        QFile f(pubPath);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, tr("Key install failed"),
                                  tr("Cannot read file:\n%1").arg(f.errorString()));
            return;
        }
        const QString pubLine = QString::fromUtf8(f.readAll()).trimmed();
        f.close();

        onInstallPublicKeyRequested(pubLine, row);
    });

    QAction *testUnlockAct = new QAction(tr("DEV: Test Dilithium key unlock…"), this);
    testUnlockAct->setToolTip(tr("Dev tool: decrypt .enc key and validate format (only visible when PQ debug is ON)."));
    keysMenu->addAction(testUnlockAct);

    auto syncDevVisibility = [this, testUnlockAct]() {
        const bool on = (m_pqDebugCheck && m_pqDebugCheck->isChecked());
        testUnlockAct->setVisible(on);
        testUnlockAct->setEnabled(on);
    };
    syncDevVisibility();

    if (m_pqDebugCheck) {
        connect(m_pqDebugCheck, &QCheckBox::toggled, this, [syncDevVisibility](bool) {
            syncDevVisibility();
        });
    }

    connect(testUnlockAct, &QAction::triggered, this, [this]() {
        if (!(m_pqDebugCheck && m_pqDebugCheck->isChecked())) {
            appendTerminalLine(tr("[DEV] PQ debug is OFF. Enable it to use the tester."));
            return;
        }

        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Select encrypted Dilithium key (.enc)"),
            QDir(QDir::homePath()).filePath(".pq-ssh/keys"),
            tr("Encrypted keys (*.enc);;All files (*)")
        );
        if (path.isEmpty()) return;

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            appendTerminalLine(tr("[TEST] ❌ Cannot read file: %1").arg(f.errorString()));
            return;
        }
        const QByteArray enc = f.readAll();
        f.close();

        appendTerminalLine(tr("[TEST] File: %1").arg(QFileInfo(path).fileName()));
        appendTerminalLine(tr("[TEST] Enc size: %1 bytes").arg(enc.size()));

        const int minSize =
            6 + crypto_pwhash_SALTBYTES + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES +
            crypto_aead_xchacha20poly1305_ietf_ABYTES;

        if (enc.size() < minSize) {
            appendTerminalLine(tr("[TEST] ❌ Too small to be valid (need >= %1 bytes)").arg(minSize));
            return;
        }

        const QByteArray magic = enc.left(6);
        appendTerminalLine(tr("[TEST] Magic: '%1'").arg(QString::fromLatin1(magic)));
        if (magic != "PQSSH1") {
            appendTerminalLine(tr("[TEST] ⚠ WARN: Magic mismatch (expected 'PQSSH1'). Will still try decrypt."));
        }

        QString err;
        const bool ok = m_ssh.testUnlockDilithiumKey(path, &err);
        if (ok) {
            appendTerminalLine(tr("[TEST] ✅ Unlock OK: %1").arg(QFileInfo(path).fileName()));

            const QByteArray encSha = QCryptographicHash::hash(enc, QCryptographicHash::Sha256).toHex();
            appendTerminalLine(tr("[TEST] Enc SHA256: %1").arg(QString::fromLatin1(encSha)));
        } else {
            appendTerminalLine(tr("[TEST] ❌ Unlock FAILED: %1").arg(err));
        }
    });

    // View
    auto *viewMenu = menuBar()->addMenu(tr("&View"));

    QAction *identityAct = new QAction(tr("Identity manager…"), this);
    identityAct->setToolTip(tr("Recover a global SSH keypair from 24 words (Ed25519)."));
    viewMenu->addAction(identityAct);
    connect(identityAct, &QAction::triggered, this, &MainWindow::onIdentityManagerRequested);

    // Help
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));

    QAction *auditViewerAct = new QAction(tr("Audit log viewer…"), this);
    auditViewerAct->setToolTip(tr("View audit logs in a readable, colored format"));
    connect(auditViewerAct, &QAction::triggered, this, [this]() {
        auto* dlg = new AuditLogViewerDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addAction(auditViewerAct);

    QAction *openAuditDirAct = new QAction(tr("Open audit log folder"), this);
    openAuditDirAct->setToolTip(tr("Open audit log directory"));
    connect(openAuditDirAct, &QAction::triggered, this, []() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/audit";
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    helpMenu->addAction(openAuditDirAct);

    QAction *manualAct = new QAction(tr("User Manual"), this);
    manualAct->setToolTip(tr("Open PQ-SSH user manual"));
    connect(manualAct, &QAction::triggered, this, &MainWindow::onOpenUserManual);
    helpMenu->addAction(manualAct);

    QAction *openLogAct = new QAction(tr("Open log file"), this);
    openLogAct->setToolTip(tr("Open pq-ssh log file"));
    connect(openLogAct, &QAction::triggered, this, &MainWindow::onOpenLogFile);
    helpMenu->addAction(openLogAct);

    statusBar()->showMessage(tr("Ready"));

    if (!m_versionLabel) {
        m_versionLabel = new QLabel(statusBar());
        m_versionLabel->setStyleSheet("color:#888; padding:0 6px;");
        statusBar()->addPermanentWidget(m_versionLabel);
    }

    const QString v = QCoreApplication::applicationVersion().trimmed();
    m_versionLabel->setText(v.isEmpty() ? tr("v0.0.0") : (tr("v") + v));
    m_versionLabel->setToolTip(tr("CPUNK PQ-SSH %1").arg(v.isEmpty() ? tr("v0.0.0") : (tr("v") + v)));
}

void MainWindow::appendTerminalLine(const QString &line)
{
    const bool verbose = (m_pqDebugCheck && m_pqDebugCheck->isChecked());

    if (!verbose) {
        if (line.startsWith("[SSH-CMD]") ||
            line.startsWith("[SSH-WRAP]") ||
            line.startsWith("[TERM] available schemes:") ||
            line.startsWith("[TERM] profile=") ||
            line.startsWith("[SECURITY] Local shell fallback")) {
            qInfo().noquote() << "[UI-HIDDEN]" << line;
            return;
        }
    }

    if (m_terminal)
        m_terminal->appendPlainText(line);
}

void MainWindow::updatePqStatusLabel(const QString &text, const QString &colorHex)
{
    if (!m_pqStatusLabel) return;

    m_pqStatusLabel->setText(text);

    QString col = colorHex.trimmed();
    if (col.isEmpty()) {
        col = qApp->palette().color(QPalette::Highlight).name();
    }

    m_pqStatusLabel->setStyleSheet(QString("color:%1; font-weight:bold;").arg(col));
}

// ========================
// Profiles
// ========================
void MainWindow::loadProfiles()
{
    QString err;
    m_profiles = ProfileStore::load(&err);

    if (m_profiles.isEmpty()) {
        m_profiles = ProfileStore::defaults();
        QString saveErr;
        ProfileStore::save(m_profiles, &saveErr);
        if (!saveErr.isEmpty())
            appendTerminalLine(tr("[WARN] Could not save default profiles: %1").arg(saveErr));
    }

    rebuildProfileList();

    if (!err.isEmpty())
        appendTerminalLine(tr("[WARN] ProfileStore: %1").arg(err));
}

void MainWindow::saveProfilesToDisk()
{
    QString err;
    if (!ProfileStore::save(m_profiles, &err)) {
        appendTerminalLine(tr("[ERROR] %1").arg(err));
        if (m_statusLabel)
            m_statusLabel->setText(tr("Failed to save profiles"));
    }
}

void MainWindow::onEditProfilesClicked()
{
    if (m_profilesEditor) {
        m_profilesEditor->raise();
        m_profilesEditor->activateWindow();
        return;
    }

    ensureProfileItemSelected();
    const int selected = currentProfileIndex();

    m_profilesEditor = new ProfilesEditorDialog(m_profiles, (selected >= 0 ? selected : 0), nullptr);
    m_profilesEditor->setAttribute(Qt::WA_DeleteOnClose, true);
    m_profilesEditor->setModal(false);
    m_profilesEditor->setWindowModality(Qt::NonModal);
    m_profilesEditor->setWindowFlag(Qt::Window, true);

    connect(m_profilesEditor, &QDialog::accepted, this, [this]() {
        if (!m_profilesEditor) return;

        m_profiles = m_profilesEditor->resultProfiles();
        saveProfilesToDisk();
        rebuildProfileList();

        if (m_statusLabel)
            m_statusLabel->setText(tr("Profiles updated."));
        appendTerminalLine(tr("[INFO] Profiles updated."));
    });

    connect(m_profilesEditor, &QObject::destroyed, this, [this]() {
        m_profilesEditor = nullptr;
    });

    m_profilesEditor->show();
    m_profilesEditor->raise();
    m_profilesEditor->activateWindow();
}

void MainWindow::onInstallPublicKeyRequested(const QString& pubKeyLine, int profileIndex)
{
    if (pubKeyLine.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Key install"), tr("Public key is empty."));
        return;
    }

    if (profileIndex < 0 || profileIndex >= m_profiles.size()) {
        QMessageBox::warning(this, tr("Key install"), tr("Invalid target profile."));
        return;
    }

    const SshProfile &p = m_profiles[profileIndex];

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Key install"), tr("Profile has empty user/host."));
        return;
    }

    const int port = (p.port > 0) ? p.port : 22;

    const QString hostLine =
        (port != 22)
            ? QString("%1@%2:%3").arg(p.user, p.host).arg(port)
            : QString("%1@%2").arg(p.user, p.host);

    const QStringList parts =
        pubKeyLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const QString keyType = parts.size() >= 1 ? parts[0] : QString("unknown");
    const QString keyBlob = parts.size() >= 2 ? parts[1] : QString();
    const QString preview =
        keyBlob.isEmpty() ? QString() : (keyBlob.left(18) + "…" + keyBlob.right(10));

    const QString confirmText =
        tr("You are about to install a public key to this host:\n\n"
           "Target profile:\n  %1\n\n"
           "Host:\n  %2\n\n"
           "Remote path:\n  ~/.ssh/authorized_keys\n\n"
           "Key type:\n  %3\n\n"
           "Key preview:\n  %4\n\n"
           "Proceed?")
            .arg(p.name, hostLine, keyType, preview);

    auto btn = QMessageBox::question(
        this,
        tr("Confirm key install"),
        confirmText,
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel
    );

    if (btn != QMessageBox::Yes)
        return;

    if (!m_ssh.isConnected()) {
        QString e;
        if (!m_ssh.connectProfile(p, &e)) {
            QMessageBox::critical(this, tr("Key install failed"),
                                  tr("SSH(SFTP) connection failed:\n%1").arg(e));
            return;
        }
    }

    QString err;
    bool already = false;

    if (!m_ssh.installAuthorizedKey(pubKeyLine, &err, &already)) {
        QMessageBox::critical(this, tr("Key install failed"), err);
        return;
    }

    QMessageBox::information(
        this,
        tr("Key install"),
        already ? tr("Key already existed in authorized_keys.")
                : tr("Key installed successfully.")
    );
}

// ========================
// Connect / disconnect
// ========================
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
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QString rawHost = m_hostField ? m_hostField->text().trimmed() : QString();
    ensureProfileItemSelected();
    const int row = currentProfileIndex();

    logSessionInfo(QString("Connect clicked; profileRow=%1 rawHost='%2'")
                   .arg(row).arg(rawHost));

    if (row < 0 || row >= m_profiles.size()) {
        if (m_statusLabel) m_statusLabel->setText(tr("No profile selected."));
        uiWarn(tr("[WARN] No profile selected."));
        return;
    }

    const SshProfile p = m_profiles[row];

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText(tr("Profile has empty user/host."));
        uiWarn(tr("[WARN] Profile has empty user/host."));
        return;
    }

    const int port = (p.port > 0) ? p.port : 22;
    const QString shownTarget = QString("%1@%2").arg(p.user, p.host);

    if (m_statusLabel)
        m_statusLabel->setText(tr("Connecting to %1 ...").arg(shownTarget));

    uiInfo(tr("[CONNECT] %1 → %2:%3").arg(p.name, shownTarget).arg(port));

    logSessionInfo(QString("Using profile='%1' user='%2' host='%3' port=%4 keyType='%5' scheme='%6'")
                   .arg(p.name, p.user, p.host)
                   .arg(port)
                   .arg(p.keyType)
                   .arg(p.termColorScheme));

    const bool newWindow =
        (m_openInNewWindowCheck && m_openInNewWindowCheck->isChecked());

    openShellForProfile(p, shownTarget, newWindow);

    updatePqStatusLabel(tr("PQ: checking…"), "#888");

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

    pqArgs << shownTarget << "true";

    uiDebug(tr("[PQ-PROBE] %1").arg(prettyCommandLine("ssh", pqArgs)));

    connect(pqProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, pqProc](int exitCode, QProcess::ExitStatus st) {

                const QString err = QString::fromUtf8(pqProc->readAllStandardError());

                logSessionInfo(QString("PQ probe finished exitCode=%1 status=%2 stderrLen=%3")
                               .arg(exitCode)
                               .arg(st == QProcess::NormalExit ? "normal" : "crash")
                               .arg(err.size()));

                QString e = err;
                e.replace("\r\n", "\n");
                e.replace('\r', '\n');
                e = e.trimmed();

                if (e.isEmpty()) {
                    uiDebug(tr("[PQ-PROBE] stderr: <empty>"));
                } else {
                    const QString firstLine = e.section('\n', 0, 0);
                    uiDebug(tr("[PQ-PROBE] stderr(first): %1").arg(firstLine));
                }

                const bool pqOk =
                    !(err.contains("no matching key exchange", Qt::CaseInsensitive) ||
                      err.contains("no matching key exchange method", Qt::CaseInsensitive));

                QSettings s;
                const QString themeId = s.value("ui/theme", "cpunk-dark").toString();

                updatePqStatusLabel(
                    pqOk ? tr("PQ support: YES") : tr("PQ support: NO"),
                    pqOk ? AppTheme::accent(themeId).name()
                         : QString("#ff5252")
                );

                pqProc->deleteLater();
            });

    pqProc->start("ssh", pqArgs);

    const QString keyType =
        p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();

    if (!(keyType == "auto" || keyType == "openssh")) {
        uiWarn(tr("[SFTP] Disabled (key_type='%1' not supported yet)").arg(keyType));
        logSessionInfo(QString("SFTP disabled due to unsupported key_type='%1'").arg(keyType));
        if (m_filesTab) m_filesTab->onSshDisconnected();
    } else {
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);

        connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished,
                this, [this, watcher, p, port]() {

                    const auto res = watcher->result();
                    const bool ok = res.first;
                    const QString err = res.second;

                    if (ok) {
                        uiInfo(tr("[SFTP] Ready (%1@%2:%3)").arg(p.user, p.host).arg(port));
                        logSessionInfo("libssh connected OK (SFTP ready)");

                        if (m_filesTab) {
                            m_filesTab->onSshConnected();
                        }
                    } else {
                        uiWarn(tr("[SFTP] Disabled (libssh connect failed: %1)").arg(err));
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

    if (m_connectBtn)    m_connectBtn->setEnabled(false);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(true);

    if (m_statusLabel)
        m_statusLabel->setText(tr("Connected: %1").arg(shownTarget));
}

void MainWindow::onProfileDoubleClicked()
{
    ensureProfileItemSelected();
    if (currentProfileIndex() < 0) return;
    onConnectClicked();
}

void MainWindow::onDisconnectClicked()
{
    logSessionInfo("Disconnect clicked (user requested)");
    m_ssh.disconnect();
    if (m_filesTab) m_filesTab->onSshDisconnected();
    if (m_connectBtn)    m_connectBtn->setEnabled(true);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(false);

    if (m_statusLabel) m_statusLabel->setText(tr("Disconnected."));
}

void MainWindow::onSendInput()
{
    const QString text = m_inputField ? m_inputField->text().trimmed() : QString();
    if (text.isEmpty()) return;

    appendTerminalLine(tr("> %1").arg(text));
    appendTerminalLine(tr("[INFO] Shell not implemented yet; input not sent anywhere."));

    if (m_inputField) m_inputField->clear();
}

// ========================
// Drag-drop upload
// ========================
void MainWindow::onFileDropped(const QString &path, const QByteArray &data)
{
    QFileInfo info(path);
    const QString fileName = info.fileName().isEmpty()
                                 ? QStringLiteral("dropped_file")
                                 : info.fileName();

    appendTerminalLine(tr("[DROP] %1 (%2 bytes)").arg(fileName).arg(data.size()));

    if (!m_ssh.isConnected()) {
        QDir baseDir(QDir::homePath() + "/pqssh_drops");
        if (!baseDir.exists()) baseDir.mkpath(".");
        const QString outPath = baseDir.filePath(fileName);

        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            appendTerminalLine(tr("[DROP] ERROR: %1").arg(out.errorString()));
            return;
        }
        out.write(data);
        out.close();

        appendTerminalLine(tr("[DROP] Saved locally: %1").arg(outPath));
        return;
    }

    QString err;
    const QString pwd = m_ssh.remotePwd(&err);
    if (pwd.isEmpty()) {
        appendTerminalLine(tr("[UPLOAD] Could not read remote pwd. %1").arg(err));
        return;
    }

    const QString remotePath = pwd + "/" + fileName;
    if (!m_ssh.uploadBytes(remotePath, data, &err)) {
        appendTerminalLine(tr("[UPLOAD] FAILED: %1").arg(err));
        return;
    }

    appendTerminalLine(tr("[UPLOAD] OK → %1").arg(remotePath));
}

void MainWindow::downloadSelectionTriggered()
{
    appendTerminalLine(tr("[DOWNLOAD] Shell not implemented yet (no selection source)."));
}

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
CpunkTermWidget* MainWindow::createTerm(const SshProfile &p, QWidget *parent)
{
    auto *term = new CpunkTermWidget(0, parent);
    term->setHistorySize(qMax(0, p.historyLines));

    applyProfileToTerm(term, p);

    QStringList sshArgs;
    sshArgs << "-tt";
    if (p.pqDebug) sshArgs << "-vv";

    sshArgs << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    sshArgs << "-o" << "ConnectTimeout=5";
    sshArgs << "-o" << "ConnectionAttempts=1";

    sshArgs << "-o" << "StrictHostKeyChecking=accept-new";
    sshArgs << "-o" << ("UserKnownHostsFile=" + QDir::homePath() + "/.ssh/known_hosts");

    const QString kt = p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();
    const bool hasKeyFile = !p.keyFile.trimmed().isEmpty();

    if (kt == "auto" || kt == "openssh") {
        sshArgs << "-o" << "PubkeyAuthentication=yes";
        sshArgs << "-o" << "IdentitiesOnly=yes";
        sshArgs << "-o" << "PreferredAuthentications=publickey,password,keyboard-interactive";
        sshArgs << "-o" << "PasswordAuthentication=yes";
        sshArgs << "-o" << "KbdInteractiveAuthentication=yes";
        sshArgs << "-o" << "NumberOfPasswordPrompts=3";
        sshArgs << "-o" << "GSSAPIAuthentication=no";
        sshArgs << "-o" << "HostbasedAuthentication=no";

        sshArgs << "-o" << "ControlMaster=no";
        sshArgs << "-o" << "ControlPath=none";
        sshArgs << "-o" << "ControlPersist=no";

        if (hasKeyFile)
            sshArgs << "-i" << p.keyFile.trimmed();
    } else {
        appendTerminalLine(tr("[SSH] key_type='%1' not implemented yet → falling back to password auth.")
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

    appendTerminalLine(tr("[SSH-CMD] ssh %1").arg(sshArgs.join(" ")));

    auto shQuote = [](const QString &s) -> QString {
        QString out = s;
        out.replace("'", "'\"'\"'");
        return "'" + out + "'";
    };

    QString cmd = "exec ssh";
    for (const QString &a : sshArgs)
        cmd += " " + shQuote(a);

    appendTerminalLine(tr("[SSH-WRAP] %1").arg(cmd));
    appendTerminalLine(tr("[SECURITY] Local shell fallback disabled (exec ssh)"));

    term->setShellProgram("/bin/bash");
    term->setArgs(QStringList() << "-lc" << cmd);

    term->setAutoClose(true);
    term->startShellProgram();

    QTimer::singleShot(0,  term, [term]() { protectTermFromAppStyles(term); });
    QTimer::singleShot(50, term, [term]() { protectTermFromAppStyles(term); });

    const QString scheme = p.termColorScheme.isEmpty() ? "WhiteOnBlack" : p.termColorScheme;
    if (scheme == "WhiteOnBlack") {
        QTimer::singleShot(0,  term, [term]() { forceBlackBackground(term); });
        QTimer::singleShot(50, term, [term]() { forceBlackBackground(term); });
    }

    connect(term, &CpunkTermWidget::fileDropped,
            this, &MainWindow::onFileDropped);

    connect(term, &QTermWidget::finished, this, [this, term]() {
        appendTerminalLine(tr("[TERM] ssh ended; closing terminal tab/window and disconnecting."));

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

    appendTerminalLine(tr("[TERM] ssh started (wrapped); terminal will close when ssh exits."));
    return term;
}

static void forceBlackBackground(CpunkTermWidget *term)
{
    if (!term) return;

    term->setAutoFillBackground(true);
    term->setStyleSheet("background: #000; color: #fff;");

    QPalette pal = term->palette();
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::Base,   Qt::black);
    pal.setColor(QPalette::Text,   Qt::white);
    term->setPalette(pal);

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

    term->update();
}

void MainWindow::applyProfileToTerm(CpunkTermWidget *term, const SshProfile &p)
{
    if (!term) return;

    const QString scheme =
        p.termColorScheme.isEmpty() ? "WhiteOnBlack" : p.termColorScheme;

    appendTerminalLine(tr("[TERM] profile=%1 scheme='%2' (raw='%3')")
                       .arg(p.name, scheme, p.termColorScheme));
    appendTerminalLine(tr("[TERM] available schemes: %1")
                       .arg(term->availableColorSchemes().join(", ")));

    term->setColorScheme(scheme);

    protectTermFromAppStyles(term);

    QFont f("Monospace");
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(p.termFontSize > 0 ? p.termFontSize : 11);
    term->setTerminalFont(f);

    term->setTerminalOpacity(1.0);

    if (scheme == "WhiteOnBlack") {
        forceBlackBackground(term);
        protectTermFromAppStyles(term);
    }

    syncTerminalSurroundingsToTerm(term);
}

void MainWindow::openShellForProfile(const SshProfile &p, const QString &target, bool newWindow)
{
    Q_UNUSED(target);

    const int port = (p.port > 0) ? p.port : 22;

    const QString connLabel = (port != 22)
        ? QString("%1@%2:%3").arg(p.user, p.host).arg(port)
        : QString("%1@%2").arg(p.user, p.host);

    const QString windowTitle = tr("PQ-SSH: %1 (%2)").arg(p.name, connLabel);
    const QString tabTitle    = tr("%1 (%2)").arg(p.name, connLabel);

    if (newWindow) {
        auto *w = new QMainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose, true);
        w->setWindowTitle(windowTitle);
        w->resize(p.termWidth > 0 ? p.termWidth : 900,
                  p.termHeight > 0 ? p.termHeight : 500);

        auto *term = createTerm(p, w);
        w->setCentralWidget(term);

        w->setProperty("pqssh_term", QVariant::fromValue(static_cast<QObject*>(term)));
        w->installEventFilter(this);

        installHotkeyMacro(term, w, p);

        connect(w, &QObject::destroyed, this, [this]() {
            onDisconnectClicked();
        });

        w->show();
        w->raise();
        w->activateWindow();
        focusTerminalWindow(w, term);
        return;
    }

    if (!m_tabbedShellWindow) {
        m_tabbedShellWindow = new QMainWindow();
        m_tabbedShellWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_tabbedShellWindow->setWindowTitle(tr("PQ-SSH Tabs"));

        m_tabbedShellWindow->installEventFilter(this);

        m_tabWidget = new QTabWidget(m_tabbedShellWindow);
        m_tabWidget->setTabsClosable(true);
        m_tabbedShellWindow->setCentralWidget(m_tabWidget);

        connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, [this](int idx) {
            if (!m_tabWidget) return;

            QWidget *w = m_tabWidget->widget(idx);
            m_tabWidget->removeTab(idx);

            if (auto *term = qobject_cast<CpunkTermWidget*>(w))
                stopTerm(term);

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

    auto *term = createTerm(p, m_tabWidget);
    const int idx = m_tabWidget->addTab(term, tabTitle);
    m_tabWidget->setCurrentIndex(idx);

    installHotkeyMacro(term, m_tabbedShellWindow, p);

    m_tabbedShellWindow->setWindowTitle(tr("PQ-SSH Tabs — %1").arg(connLabel));

    m_tabbedShellWindow->show();
    m_tabbedShellWindow->raise();
    m_tabbedShellWindow->activateWindow();
    focusTerminalWindow(m_tabbedShellWindow, term);
}

static void protectTermFromAppStyles(CpunkTermWidget *term)
{
    if (!term) return;

    const QString shield =
        "QWidget { background-color: palette(Base); color: palette(Text); }";

    term->setAutoFillBackground(true);
    term->setStyleSheet(shield);

    const auto kids = term->findChildren<QWidget*>();
    for (QWidget *w : kids) {
        w->setAutoFillBackground(true);
        w->setStyleSheet(shield);
    }
}

void MainWindow::onOpenLogFile()
{
    const QString path = Logger::logFilePath();

    if (path.isEmpty()) {
        appendTerminalLine(tr("[LOG] Log file path not available."));
        qWarning() << "Log file path is empty";
        return;
    }

    const QUrl url = QUrl::fromLocalFile(path);
    QDesktopServices::openUrl(url);

    qInfo() << "Opened log file:" << path;
}

void MainWindow::logSessionInfo(const QString& msg)
{
    const QString sid = m_sessionId.isEmpty() ? "-" : m_sessionId;
    qInfo().noquote() << QString("[SESSION %1] %2").arg(sid, msg);
}

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
    const bool verbose = (m_pqDebugCheck && m_pqDebugCheck->isChecked());
    qInfo().noquote() << "[UI-DEBUG]" << msg;
    if (verbose) appendTerminalLine(msg);
}

bool MainWindow::uiVerbose() const
{
    return (m_pqDebugCheck && m_pqDebugCheck->isChecked());
}

void MainWindow::onOpenUserManual()
{
    const QUrl url("qrc:/docs/user-manual.html");

    if (!QFile::exists(":/docs/user-manual.html")) {
        appendTerminalLine(tr("[WARN] User manual resource missing: :/docs/user-manual.html"));
        return;
    }

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowTitle(tr("PQ-SSH User Manual"));
    dlg->resize(980, 720);

    auto *layout = new QVBoxLayout(dlg);

    auto *browser = new QTextBrowser(dlg);

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

    browser->setOpenExternalLinks(true);
    browser->setSource(url);

    layout->addWidget(browser);

    dlg->setLayout(layout);
    dlg->show();
}

void MainWindow::onTestUnlockDilithiumKey()
{
    if (!(m_pqDebugCheck && m_pqDebugCheck->isChecked())) {
        appendTerminalLine(tr("[DEV] PQ debug is OFF. Enable it to use the tester."));
        return;
    }

    const QString startDir = QDir(QDir::homePath()).filePath(".pq-ssh/keys");
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select encrypted Dilithium private key"),
        startDir,
        tr("Encrypted keys (*.enc);;All files (*)")
    );
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("DEV test failed"),
                             tr("Cannot read file:\n%1").arg(f.errorString()));
        return;
    }
    const QByteArray enc = f.readAll();
    f.close();

    if (enc.size() < (6 + crypto_pwhash_SALTBYTES + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + 16)) {
        QMessageBox::warning(this, tr("DEV test failed"),
                             tr("File is too small to be a valid PQSSH encrypted key.\n"
                                "Expected: MAGIC(6)+SALT(16)+NONCE(24)+CIPHERTEXT(tag...)."));
        return;
    }

    const QByteArray magic = enc.left(6);
    appendTerminalLine(tr("[DEV] Selected: %1").arg(QFileInfo(path).fileName()));
    appendTerminalLine(tr("[DEV] Enc size: %1 bytes").arg(enc.size()));
    appendTerminalLine(tr("[DEV] Magic: '%1'").arg(QString::fromLatin1(magic)));

    if (magic != "PQSSH1") {
        appendTerminalLine(tr("[DEV][WARN] Magic mismatch. Expected 'PQSSH1'. Continuing anyway to test decrypt..."));
    }

    bool ok = false;
    QString pass;
    {
        bool localOk = false;
        pass = QInputDialog::getText(this,
                                     tr("Dilithium Key Passphrase"),
                                     tr("Enter passphrase for:\n%1").arg(QFileInfo(path).fileName()),
                                     QLineEdit::Password,
                                     QString(),
                                     &localOk);
        ok = localOk;
    }

    if (!ok) {
        appendTerminalLine(tr("[DEV] Cancelled by user."));
        return;
    }

    QByteArray plain;
    QString decErr;
    if (!decryptDilithiumKey(enc, pass, &plain, &decErr)) {
        appendTerminalLine(tr("[DEV][FAIL] Decrypt failed: %1").arg(decErr));
        QMessageBox::warning(this, tr("DEV test failed"), tr("Decrypt failed:\n%1").arg(decErr));
        return;
    }

    if (plain.isEmpty()) {
        appendTerminalLine(tr("[DEV][FAIL] Decrypt returned empty plaintext."));
        QMessageBox::warning(this, tr("DEV test failed"), tr("Decrypt returned empty plaintext."));
        return;
    }

    const QByteArray digest = QCryptographicHash::hash(plain, QCryptographicHash::Sha256).toHex();
    appendTerminalLine(tr("[DEV][OK] Decrypted plaintext size: %1 bytes").arg(plain.size()));
    appendTerminalLine(tr("[DEV][OK] Plain SHA256: %1").arg(QString::fromLatin1(digest)));

    if (plain.size() < 2048) {
        appendTerminalLine(tr("[DEV][WARN] Plaintext is unexpectedly small for Dilithium private key material."));
    }

    sodium_memzero(plain.data(), (size_t)plain.size());

    QMessageBox::information(this, tr("DEV test OK"),
                             tr("Decrypt OK.\n\n"
                                "Validated format + passphrase unlock.\n"
                                "Details written to the terminal/log."));
}

void MainWindow::applySavedSettings()
{
    QSettings s;

    applyCurrentTheme();

    Logger::setLogLevel(s.value("logging/level", 1).toInt());

    const QString logFile = s.value("logging/filePath", "").toString().trimmed();
    Logger::setLogFilePathOverride(logFile);

    const QString auditDir = s.value("audit/dirPath", "").toString().trimmed();
    AuditLogger::setAuditDirOverride(auditDir);
}

void MainWindow::onOpenSettingsDialog()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    applySavedSettings();
    rebuildProfileList();

    appendTerminalLine(tr("[INFO] Settings updated."));
    if (m_statusLabel) m_statusLabel->setText(tr("Settings updated."));
}

void MainWindow::onOpenSettings()
{
    QMessageBox::information(this, tr("Settings"), tr("Settings dialog (coming next)."));
}

void MainWindow::installHotkeyMacro(CpunkTermWidget* term, QWidget* shortcutScope, const SshProfile& p)
{
    if (!term || !shortcutScope) return;

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

    QPointer<CpunkTermWidget> termPtr(term);

    for (int i = 0; i < macros.size(); ++i) {
        const ProfileMacro& m = macros[i];

        const QString shortcutText = m.shortcut.trimmed();
        const QString cmd          = m.command;
        const bool sendEnter       = m.sendEnter;

        if (shortcutText.isEmpty() || cmd.trimmed().isEmpty())
            continue;

        const QKeySequence seq(shortcutText);
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (seq.isEmpty()) {
            uiWarn(tr("[MACRO] Invalid shortcut '%1' (profile '%2')").arg(shortcutText, p.name));
            continue;
        }
#endif

        auto* sc = new QShortcut(seq, shortcutScope);
        sc->setContext(Qt::WidgetWithChildrenShortcut);

        const QString shownName = m.name.trimmed().isEmpty()
            ? tr("Macro %1").arg(i + 1)
            : m.name.trimmed();

        uiDebug(tr("[MACRO] Bound %1 → \"%2\" (enter=%3) [%4] profile '%5'")
                    .arg(seq.toString(),
                         cmd.trimmed(),
                         sendEnter ? tr("yes") : tr("no"),
                         shownName,
                         p.name));

        connect(sc, &QShortcut::activated, this, [this, termPtr, cmd, sendEnter, shownName]() {
            if (!termPtr) return;

            QString out = cmd;
            if (sendEnter)
                out += "\n";

            termPtr->sendText(out);
            uiDebug(tr("[MACRO] Sent (%1): %2").arg(shownName, cmd.trimmed()));
        });
    }
}

void MainWindow::onKexNegotiated(const QString& prettyText, const QString& rawKex)
{
    const bool pq =
        rawKex.contains("mlkem", Qt::CaseInsensitive) ||
        rawKex.contains("sntrup", Qt::CaseInsensitive);

    updatePqStatusLabel(
        pq ? (tr("PQ KEX: %1").arg(prettyText)) : (tr("KEX: %1").arg(prettyText)),
        pq ? "#00FF99" : "#9AA0A6"
    );

    if (m_pqStatusLabel)
        m_pqStatusLabel->setToolTip(tr("Negotiated KEX: %1").arg(rawKex));
}

void MainWindow::onIdentityManagerRequested()
{
    if (m_identityDlg) {
        m_identityDlg->raise();
        m_identityDlg->activateWindow();
        return;
    }

    m_identityDlg = new IdentityManagerDialog(nullptr);
    m_identityDlg->setAttribute(Qt::WA_DeleteOnClose, true);
    m_identityDlg->setWindowTitle(tr("CPUNK PQ-SSH — Identity Manager"));

    connect(m_identityDlg, &QObject::destroyed, this, [this]() {
        m_identityDlg = nullptr;
    });

    m_identityDlg->show();
    m_identityDlg->raise();
    m_identityDlg->activateWindow();
}

void MainWindow::onImportOpenSshConfig()
{
    if (m_sshPlanDlg) {
        m_sshPlanDlg->raise();
        m_sshPlanDlg->activateWindow();
        return;
    }

    const QString sshDir = QDir(QDir::homePath()).filePath(".ssh");
    const QString path   = QDir(sshDir).filePath("config");

    QDir().mkpath(sshDir);

    if (!QFileInfo::exists(path)) {
        const auto ans = QMessageBox::question(
            this,
            tr("OpenSSH config not found"),
            tr("No OpenSSH config file was found at:\n\n%1\n\nCreate a starter ~/.ssh/config now?")
                .arg(path),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes
        );

        if (ans != QMessageBox::Yes)
            return;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QMessageBox::critical(this, tr("Cannot create config"),
                                  tr("Failed to create:\n%1\n\n%2").arg(path, f.errorString()));
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
                    tr("Nothing to import yet"),
                    tr("Your ~/.ssh/config contains no active Host entries (only comments).\n\n"
                       "Add a block like:\n\n"
                       "Host myserver\n"
                       "    HostName 192.168.1.10\n"
                       "    User root\n"
                       "    Port 22\n\n"
                       "Then try Import again.")
                );
            }
        }
    }

    const SshConfigParseResult parsed = SshConfigParser::parseFile(path);

    QStringList existingNames;
    existingNames.reserve(m_profiles.size());
    for (const auto& p : m_profiles)
        existingNames << p.name;

    m_sshPlanDlg = new SshConfigImportPlanDialog(path, parsed, existingNames, nullptr);
    m_sshPlanDlg->setAttribute(Qt::WA_DeleteOnClose, true);
    m_sshPlanDlg->setWindowTitle(tr("CPUNK PQ-SSH — Import Plan"));

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
    Q_UNUSED(updates);

    int added = 0;

    for (const auto& ip : creates) {
        SshProfile p;
        p.name = ip.name;
        p.host = ip.hostName;
        p.user = ip.user;
        p.port = ip.port;

        m_profiles.push_back(p);
        added++;
    }

    if (added > 0) {
        saveProfilesToDisk();
        rebuildProfileList();
        appendTerminalLine(tr("[INFO] Imported %1 profile(s) from ~/.ssh/config").arg(added));
        if (m_statusLabel) m_statusLabel->setText(tr("Imported %1 profiles").arg(added));
    } else {
        appendTerminalLine(tr("[INFO] Import plan applied: no profiles added."));
        if (m_statusLabel) m_statusLabel->setText(tr("Import plan applied: no changes."));
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_tabWidget) {
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (auto *term = qobject_cast<CpunkTermWidget*>(m_tabWidget->widget(i))) {
                stopTerm(term);
            }
        }
    }

    AuditLogger::writeEvent("session.end");
    QMainWindow::closeEvent(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev)
{
    if (ev->type() == QEvent::Close) {

        if (auto *w = qobject_cast<QWidget*>(obj)) {
            const QVariant v = w->property("pqssh_term");
            if (v.isValid()) {
                auto *term = qobject_cast<CpunkTermWidget*>(v.value<QObject*>());
                if (term) stopTerm(term);
            }
        }

        if (obj == m_tabbedShellWindow && m_tabWidget) {
            for (int i = 0; i < m_tabWidget->count(); ++i) {
                if (auto *term = qobject_cast<CpunkTermWidget*>(m_tabWidget->widget(i))) {
                    stopTerm(term);
                }
            }
        }
    }

    return QMainWindow::eventFilter(obj, ev);
}

static void stopTerm(CpunkTermWidget* term)
{
    if (!term) return;
    term->sendText("exit\n");
    term->close();
}

bool MainWindow::verifyAppPassword(const QString& pass) const
{
    if (pass.isEmpty())
        return false;

    QSettings s;
    const QByteArray hash = s.value("appLock/hash").toByteArray();
    if (hash.isEmpty())
        return false;

    return crypto_pwhash_str_verify(
               hash.constData(),
               pass.toUtf8().constData(),
               pass.toUtf8().size()
           ) == 0;
}

bool MainWindow::showStartupUnlockDialog()
{
    QDialog dlg(nullptr);
    dlg.setWindowTitle(tr("Unlock CPUNK PQ-SSH"));
    dlg.setModal(true);
    dlg.resize(360, 140);

    auto *layout = new QVBoxLayout(&dlg);

    auto *label = new QLabel(
        tr("This application is protected.\n\n"
           "Enter your application password:"),
        &dlg
    );

    auto *edit = new QLineEdit(&dlg);
    edit->setEchoMode(QLineEdit::Password);
    edit->setPlaceholderText(tr("Password"));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dlg
    );

    layout->addWidget(label);
    layout->addWidget(edit);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
        if (verifyAppPassword(edit->text())) {
            dlg.accept();
        } else {
            QMessageBox::critical(
                &dlg,
                tr("Unlock failed"),
                tr("Incorrect password.")
            );
            edit->clear();
            edit->setFocus();
        }
    });

    connect(buttons, &QDialogButtonBox::rejected,
            &dlg, &QDialog::reject);

    edit->setFocus();
    return (dlg.exec() == QDialog::Accepted);
}
