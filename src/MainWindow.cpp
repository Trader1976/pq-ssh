#include "MainWindow.h"
#include "KeyGeneratorDialog.h"
#include "KeyMetadataUtils.h"

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


// =====================================================
// Forward declarations
// =====================================================

class CpunkTermWidget;
static void forceBlackBackground(CpunkTermWidget *term);
static void protectTermFromAppStyles(CpunkTermWidget *term);

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
static void applyBg(QWidget* w, const QColor& bg)
{
    if (!w) return;
    w->setAutoFillBackground(true);
    QPalette p = w->palette();
    p.setColor(QPalette::Window, bg);
    w->setPalette(p);
    w->setStyleSheet(QString("background:%1;").arg(bg.name()));
}

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
// MainWindow
// =====================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // ---- Startup logs (go to pq-ssh.log via Logger::install in main.cpp) ----
    qInfo() << "MainWindow constructing";
    qInfo() << "App:" << QCoreApplication::applicationName()
            << "Version:" << QCoreApplication::applicationVersion();
    qInfo() << "Qt:" << QT_VERSION_STR
            << "OS:" << QSysInfo::prettyProductName()
            << "Platform:" << QGuiApplication::platformName();

    qApp->setStyleSheet(AppTheme::dark());

    setWindowTitle("CPUNK PQ-SSH");
    resize(1100, 700);

    setupUi();
    setupMenus();
    loadProfiles();

    qInfo() << "UI ready; profiles loaded";

    // ---- Startup security warning: expired keys ----
    {
        const QString metaPath = QDir(QDir::homePath()).filePath(".pq-ssh/keys/metadata.json");
        qInfo() << "Checking key metadata:" << metaPath;

        // 1) Auto-mark expired keys in metadata.json (so UI/status stays consistent)
        QString autoErr;
        autoExpireMetadataFile(metaPath, &autoErr);

        if (!autoErr.isEmpty()) {
            appendTerminalLine("[WARN] " + autoErr);
            qWarning() << "autoExpireMetadataFile:" << autoErr;
        }

        // 2) Now count expired keys (after auto-marking)
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

            if (m_statusLabel) {
                m_statusLabel->setText(msg);
                m_statusLabel->setStyleSheet("color: #ff5252; font-weight: bold;");
            }

            appendTerminalLine("[SECURITY] " + msg);
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
// UI
// ========================

void MainWindow::setupUi()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1); // thinner handle
    setCentralWidget(splitter);

    // ============================
    // Left: profiles
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
    // Right: bars + terminal
    // ============================
    auto *rightWidget = new QWidget(splitter);

    // Outer layout keeps padding for top/input/bottom bars
    auto *outer = new QVBoxLayout(rightWidget);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    // --- Top bar ---
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

    // --- Terminal container (no padding so no gutters show) ---
    auto *termContainer = new QWidget(rightWidget);
    auto *termLayout = new QVBoxLayout(termContainer);
    termLayout->setContentsMargins(0, 0, 0, 0);
    termLayout->setSpacing(0);

    // Terminal log (placeholder). In the "real" app this may be CpunkTermWidget instead.
    m_terminal = new QPlainTextEdit(termContainer);
    m_terminal->setReadOnly(true);

    // Remove frames/margins that can show black borders
    m_terminal->setFrameShape(QFrame::NoFrame);
    m_terminal->setLineWidth(0);
    m_terminal->setContentsMargins(0, 0, 0, 0);
    m_terminal->document()->setDocumentMargin(0);

    // Qt5-safe: affect the viewport directly (setViewportMargins is protected in Qt5)
    m_terminal->viewport()->setAutoFillBackground(true);
    m_terminal->viewport()->setContentsMargins(0, 0, 0, 0);

    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(11);
    m_terminal->setFont(terminalFont);

    termLayout->addWidget(m_terminal, 1);
    termContainer->setLayout(termLayout);

    // --- Input bar ---
    auto *inputBar = new QWidget(rightWidget);
    auto *inputLayout = new QHBoxLayout(inputBar);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(6);

    auto *inputLabel = new QLabel("Input:", inputBar);
    m_inputField = new QLineEdit(inputBar);
    m_sendBtn = new QPushButton("Send", inputBar);

    inputLayout->addWidget(inputLabel);
    inputLayout->addWidget(m_inputField, 1);
    inputLayout->addWidget(m_sendBtn);
    inputBar->setLayout(inputLayout);

    // --- Bottom bar ---
    auto *bottomBar = new QWidget(rightWidget);
    auto *bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);

    m_statusLabel = new QLabel("Ready.", bottomBar);
    m_statusLabel->setStyleSheet("color: gray;");

    m_pqStatusLabel = new QLabel("PQ: unknown", bottomBar);
    m_pqStatusLabel->setStyleSheet("color: #888; font-weight: bold;");

    m_pqDebugCheck = new QCheckBox("PQ debug", bottomBar);
    m_pqDebugCheck->setChecked(true);

    m_openInNewWindowCheck = new QCheckBox("Open new connection in NEW window", bottomBar);
    m_openInNewWindowCheck->setChecked(false);

    bottomLayout->addWidget(m_statusLabel, 1);
    bottomLayout->addWidget(m_openInNewWindowCheck, 0);
    bottomLayout->addWidget(m_pqStatusLabel, 0);
    bottomLayout->addWidget(m_pqDebugCheck, 0);
    bottomBar->setLayout(bottomLayout);

    // Assemble right side
    outer->addWidget(topBar);
    outer->addWidget(termContainer, 1);
    outer->addWidget(inputBar);
    outer->addWidget(bottomBar);
    rightWidget->setLayout(outer);

    // Add panes to splitter
    splitter->addWidget(profilesWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // Optional: ensure splitter handle isn't black in dark theme
    splitter->setStyleSheet("QSplitter::handle { background-color: #121212; }");

    // ============================
    // Wiring
    // ============================
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
}


void MainWindow::setupMenus()
{
    auto *fileMenu = menuBar()->addMenu("&File");

    QAction *downloadSel = new QAction("Download Selection", this);
    downloadSel->setShortcut(QKeySequence("Ctrl+D"));
    fileMenu->addAction(downloadSel);

    connect(downloadSel, &QAction::triggered,
            this, &MainWindow::downloadSelectionTriggered);

    // Keys menu
    auto *keysMenu = menuBar()->addMenu("&Keys");
    QAction *keyGenAct = new QAction("Key Generator...", this);
    keysMenu->addAction(keyGenAct);

    connect(keyGenAct, &QAction::triggered, this, [this]() {
        KeyGeneratorDialog dlg(this);
        dlg.exec();
    });

    // View menu (placeholder for future)
    menuBar()->addMenu("&View");

    // Help menu
    auto *helpMenu = menuBar()->addMenu("&Help");

    QAction *openLogAct = new QAction("Open log file", this);
    openLogAct->setToolTip("Open pq-ssh log file");
    connect(openLogAct, &QAction::triggered,
            this, &MainWindow::onOpenLogFile);

    helpMenu->addAction(openLogAct);

    statusBar()->showMessage("CPUNK PQ-SSH prototype");
}


void MainWindow::appendTerminalLine(const QString &line)
{
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
    if (m_terminal)
        m_terminal->appendPlainText(line);
}

void MainWindow::updatePqStatusLabel(const QString &text, const QString &colorHex)
{
    if (!m_pqStatusLabel) return;
    m_pqStatusLabel->setText(text);
    m_pqStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(colorHex));
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
            appendTerminalLine("[WARN] Could not save default profiles: " + saveErr);
    }

    if (m_profileList) {
        m_profileList->clear();
        for (const auto &p : m_profiles)
            m_profileList->addItem(p.name);

        if (!m_profiles.isEmpty()) {
            m_profileList->setCurrentRow(0);
            onProfileSelectionChanged(0);
        }
    }

    if (!err.isEmpty())
        appendTerminalLine("[WARN] ProfileStore: " + err);
}

void MainWindow::saveProfilesToDisk()
{
    QString err;
    if (!ProfileStore::save(m_profiles, &err)) {
        appendTerminalLine("[ERROR] " + err);
        if (m_statusLabel)
            m_statusLabel->setText("Failed to save profiles");
    }
}

void MainWindow::onEditProfilesClicked()
{
    ProfilesEditorDialog dlg(m_profiles, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_profiles = dlg.resultProfiles();
    saveProfilesToDisk();
    loadProfiles();

    if (m_statusLabel)
        m_statusLabel->setText("Profiles updated.");
    appendTerminalLine("[INFO] Profiles updated.");
}

// ========================
// Connect / disconnect
// ========================

void MainWindow::onProfileSelectionChanged(int row)
{
    if (row < 0 || row >= m_profiles.size())
        return;

    const SshProfile &p = m_profiles[row];

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
    // New session id per connect attempt
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QString rawHost = m_hostField ? m_hostField->text().trimmed() : QString();
    const int row = m_profileList ? m_profileList->currentRow() : -1;

    logSessionInfo(QString("Connect clicked; profileRow=%1 rawHost='%2'")
                   .arg(row).arg(rawHost));

    if (row < 0 || row >= m_profiles.size()) {
        if (m_statusLabel) m_statusLabel->setText("No profile selected.");
        uiWarn("[WARN] No profile selected.");
        return;
    }

    const SshProfile p = m_profiles[row];

    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText("Profile has empty user/host.");
        uiWarn("[WARN] Profile has empty user/host.");
        return;
    }

    const int port = (p.port > 0) ? p.port : 22;
    const QString shownTarget = QString("%1@%2").arg(p.user, p.host);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Connecting to %1 ...").arg(shownTarget));

    // ✅ Clean UI line (instead of giant [CONNECT] message)
    uiInfo(QString("[CONNECT] %1 → %2:%3").arg(p.name, shownTarget).arg(port));

    // Keep the detailed one in the log file only
    logSessionInfo(QString("Using profile='%1' user='%2' host='%3' port=%4 keyType='%5' scheme='%6'")
                   .arg(p.name, p.user, p.host)
                   .arg(port)
                   .arg(p.keyType)
                   .arg(p.termColorScheme));

    const bool newWindow =
        (m_openInNewWindowCheck && m_openInNewWindowCheck->isChecked());

    openShellForProfile(p, shownTarget, newWindow);

    updatePqStatusLabel("PQ: checking…", "#888");

    // ----------------------------
    // PQ KEX probe (OpenSSH)
    // ----------------------------
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

    // Noisy command line -> file log always, UI only if PQ debug enabled
    uiDebug(QString("[PQ-PROBE] ssh %1").arg(pqArgs.join(" ")));

    connect(pqProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, pqProc](int exitCode, QProcess::ExitStatus st) {
                const QString err = QString::fromUtf8(pqProc->readAllStandardError());

                // Log details
                logSessionInfo(QString("PQ probe finished exitCode=%1 status=%2 stderrLen=%3")
                               .arg(exitCode)
                               .arg(st == QProcess::NormalExit ? "normal" : "crash")
                               .arg(err.size()));
                uiDebug(QString("[PQ-PROBE] stderr: %1").arg(err.trimmed()));

                const bool pqOk =
                    !(err.contains("no matching key exchange", Qt::CaseInsensitive) ||
                      err.contains("no matching key exchange method", Qt::CaseInsensitive));

                updatePqStatusLabel(pqOk ? "PQ: ACTIVE" : "PQ: OFF",
                                    pqOk ? "#4caf50" : "#ff5252");

                pqProc->deleteLater();
            });

    pqProc->start("ssh", pqArgs);

    // ----------------------------
    // libssh connect (SFTP support)
    // ----------------------------
    const QString keyType =
        p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();

    if (!(keyType == "auto" || keyType == "openssh")) {
        uiWarn(QString("[SFTP] Disabled (key_type='%1' not supported yet)").arg(keyType));
        logSessionInfo(QString("SFTP disabled due to unsupported key_type='%1'").arg(keyType));
    } else {
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);

        connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished,
                this, [this, watcher, p, port]() {
                    const auto res = watcher->result();
                    const bool ok = res.first;
                    const QString err = res.second;

                    if (ok) {
                        uiInfo(QString("[SFTP] Ready (%1@%2:%3)").arg(p.user, p.host).arg(port));
                        logSessionInfo("libssh connected OK (SFTP ready)");
                    } else {
                        uiWarn(QString("[SFTP] Disabled (libssh connect failed: %1)").arg(err));
                        logSessionInfo(QString("libssh connect FAILED: %1").arg(err));
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
        m_statusLabel->setText(QString("Connected: %1").arg(shownTarget));
}


void MainWindow::onProfileDoubleClicked()
{
    onConnectClicked();
}

void MainWindow::onDisconnectClicked()
{
    logSessionInfo("Disconnect clicked (user requested)");
    m_ssh.disconnect();

    if (m_connectBtn)    m_connectBtn->setEnabled(true);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(false);

    if (m_statusLabel) m_statusLabel->setText("Disconnected.");
}

void MainWindow::onSendInput()
{
    const QString text = m_inputField ? m_inputField->text().trimmed() : QString();
    if (text.isEmpty()) return;

    appendTerminalLine(QString("> %1").arg(text));
    appendTerminalLine("[INFO] Shell not implemented yet; input not sent anywhere.");

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

    appendTerminalLine(QString("[DROP] %1 (%2 bytes)").arg(fileName).arg(data.size()));

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

void MainWindow::downloadSelectionTriggered()
{
    appendTerminalLine("[DOWNLOAD] Shell not implemented yet (no selection source).");
}

// ========================
// PQ probe
// ========================

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
    term->setHistorySize(2000);

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

    appendTerminalLine(QString("[SSH-CMD] ssh %1").arg(sshArgs.join(" ")));

    auto shQuote = [](const QString &s) -> QString {
        QString out = s;
        out.replace("'", "'\"'\"'");
        return "'" + out + "'";
    };

    QString cmd = "exec ssh";
    for (const QString &a : sshArgs)
        cmd += " " + shQuote(a);

    appendTerminalLine(QString("[SSH-WRAP] %1").arg(cmd));
    appendTerminalLine("[SECURITY] Local shell fallback disabled (exec ssh)");

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

    appendTerminalLine(QString("[TERM] profile=%1 scheme='%2' (raw='%3')")
                       .arg(p.name, scheme, p.termColorScheme));
    appendTerminalLine(QString("[TERM] available schemes: %1")
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

    // ✅ ADD THIS at the end:
    syncTerminalSurroundingsToTerm(term);
}


void MainWindow::openShellForProfile(const SshProfile &p, const QString &target, bool newWindow)
{
    Q_UNUSED(target);

    const int port = (p.port > 0) ? p.port : 22;

    const QString connLabel = (port != 22)
        ? QString("%1@%2:%3").arg(p.user, p.host).arg(port)
        : QString("%1@%2").arg(p.user, p.host);

    const QString windowTitle = QString("PQ-SSH: %1 (%2)").arg(p.name, connLabel);
    const QString tabTitle    = QString("%1 (%2)").arg(p.name, connLabel);

    if (newWindow) {
        auto *w = new QMainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose, true);
        w->setWindowTitle(windowTitle);
        w->resize(p.termWidth > 0 ? p.termWidth : 900,
                  p.termHeight > 0 ? p.termHeight : 500);

        auto *term = createTerm(p, w);
        w->setCentralWidget(term);

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

    auto *term = createTerm(p, m_tabWidget);
    const int idx = m_tabWidget->addTab(term, tabTitle);
    m_tabWidget->setCurrentIndex(idx);

    m_tabbedShellWindow->setWindowTitle(QString("PQ-SSH Tabs — %1").arg(connLabel));

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
        appendTerminalLine("[LOG] Log file path not available.");
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
    // Only show debug noise in the UI when PQ debug is enabled
    const bool verbose = (m_pqDebugCheck && m_pqDebugCheck->isChecked());
    qInfo().noquote() << "[UI-DEBUG]" << msg;          // always goes to file
    if (verbose) appendTerminalLine(msg);              // UI only if enabled
}

bool MainWindow::uiVerbose() const
{
    return (m_pqDebugCheck && m_pqDebugCheck->isChecked());
}
