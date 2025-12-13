#include "MainWindow.h"

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



class CpunkTermWidget;
static void forceBlackBackground(CpunkTermWidget *term);
static void protectTermFromAppStyles(CpunkTermWidget *term);


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qApp->setStyleSheet(AppTheme::dark());

    setWindowTitle("CPUNK PQ-SSH");
    resize(1100, 700);

    setupUi();
    setupMenus();
    loadProfiles();
}

MainWindow::~MainWindow()
{
    // Clean disconnect (libssh via SshClient)
    m_ssh.disconnect();
}

static void focusTerminalWindow(QWidget *window, QWidget *termWidget)
{
    if (!window || !termWidget) return;

    window->show();
    window->raise();
    window->activateWindow();

    // Focus after the window is actually shown/mapped
    QTimer::singleShot(0, window, [termWidget]() {
        termWidget->setFocus(Qt::OtherFocusReason);
    });

    // Some window managers need a second poke
    QTimer::singleShot(50, window, [termWidget]() {
        termWidget->setFocus(Qt::OtherFocusReason);
    });
}



void MainWindow::setupUi()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
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
    // Right: log + controls
    // ============================
    auto *rightWidget = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(6);

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

    // --- Terminal log ---
    m_terminal = new QPlainTextEdit(rightWidget);
    m_terminal->setReadOnly(true);
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(11);
    m_terminal->setFont(terminalFont);

    // --- Input bar (currently not wired to a real shell) ---
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

    rightLayout->addWidget(topBar);
    rightLayout->addWidget(m_terminal, 1);
    rightLayout->addWidget(inputBar);
    rightLayout->addWidget(bottomBar);
    rightWidget->setLayout(rightLayout);

    splitter->addWidget(profilesWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // ============================
    // Wiring
    // ============================
    connect(m_connectBtn, &QPushButton::clicked,
            this, &MainWindow::onConnectClicked);

    connect(m_disconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onDisconnectClicked);

    // itemDoubleClicked sends QListWidgetItem* -> use lambda since slot is void()
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

    menuBar()->addMenu("&View");
    menuBar()->addMenu("&Help");

    statusBar()->showMessage("CPUNK PQ-SSH prototype");
}

void MainWindow::appendTerminalLine(const QString &line)
{
    if (m_terminal)
        m_terminal->appendPlainText(line);
}

void MainWindow::updatePqStatusLabel(const QString &text, const QString &colorHex)
{
    if (!m_pqStatusLabel) return;
    m_pqStatusLabel->setText(text);
    m_pqStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(colorHex));
}

// ------------------------
// Profiles
// ------------------------

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

// ------------------------
// Connect / disconnect
// ------------------------

void MainWindow::onProfileSelectionChanged(int row)
{
    if (row < 0 || row >= m_profiles.size())
        return;

    const SshProfile &p = m_profiles[row];

    if (m_hostField) {
        QString shown = QString("%1@%2").arg(p.user, p.host);
        if (p.port > 0 && p.port != 22)
            shown += QString(":%1").arg(p.port);   // display only
        m_hostField->setText(shown);
    }

    if (m_pqDebugCheck)
        m_pqDebugCheck->setChecked(p.pqDebug);
}

void MainWindow::onConnectClicked()
{
    // ---- 1) Ensure a profile is selected ----
    const int row = m_profileList ? m_profileList->currentRow() : -1;
    if (row < 0 || row >= m_profiles.size()) {
        if (m_statusLabel) m_statusLabel->setText("No profile selected.");
        return;
    }

    const SshProfile p = m_profiles[row]; // copy: safe for async lambdas

    // ---- 2) Validate profile basics ----
    if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText("Profile has empty user/host.");
        return;
    }

    const QString shownTarget = QString("%1@%2").arg(p.user, p.host);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Connecting to %1 ...").arg(shownTarget));

    appendTerminalLine(QString("[CONNECT] Using profile '%1' (%2@%3:%4)")
                       .arg(p.name, p.user, p.host)
                       .arg(p.port));

    // ---- 3) Launch interactive terminal IMMEDIATELY (snappy UX) ----
    const bool newWindow =
        (m_openInNewWindowCheck && m_openInNewWindowCheck->isChecked());

    openShellForProfile(p, shownTarget, newWindow);

    // ---- 4) Kick PQ probe async (non-blocking) ----
    updatePqStatusLabel("PQ: checking…", "#888");

    auto *pqProc = new QProcess(this);

    QStringList pqArgs;
    pqArgs << "-o" << "KexAlgorithms=sntrup761x25519-sha512@openssh.com";
    pqArgs << "-o" << "PreferredAuthentications=none";
    pqArgs << "-o" << "PasswordAuthentication=no";
    pqArgs << "-o" << "BatchMode=yes";
    pqArgs << "-o" << "NumberOfPasswordPrompts=0";
    pqArgs << "-o" << "ConnectTimeout=3";
    pqArgs << "-o" << "ConnectionAttempts=1";

    // Important: probe same port as profile if not 22
    if (p.port > 0 && p.port != 22) {
        pqArgs << "-p" << QString::number(p.port);
    }

    pqArgs << shownTarget << "true";

    connect(pqProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, pqProc](int, QProcess::ExitStatus) {
                const QString err = QString::fromUtf8(pqProc->readAllStandardError());
                const bool pqOk =
                    !(err.contains("no matching key exchange", Qt::CaseInsensitive) ||
                      err.contains("no matching key exchange method", Qt::CaseInsensitive));

                updatePqStatusLabel(pqOk ? "PQ: ACTIVE" : "PQ: OFF",
                                    pqOk ? "#4caf50" : "#ff5252");
                pqProc->deleteLater();
            });

    pqProc->start("ssh", pqArgs);

    // ---- 5) Kick libssh connect async (non-blocking) ----
    const QString keyType =
        p.keyType.trimmed().isEmpty() ? QStringLiteral("auto") : p.keyType.trimmed();

    if (!(keyType == "auto" || keyType == "openssh")) {
        appendTerminalLine(QString("[SSH] key_type='%1' not supported yet for libssh (PQ auth pending) → SFTP disabled.")
                           .arg(keyType));
    } else {
        // run in background thread so UI doesn't freeze
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);

        connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished,
                this, [this, watcher, p]() {
                    const auto res = watcher->result();
                    const bool ok = res.first;
                    const QString err = res.second;

                    if (ok) {
                        appendTerminalLine(QString("[SSH] libssh connected (%1@%2:%3)")
                                           .arg(p.user, p.host)
                                           .arg(p.port));
                    } else {
                        appendTerminalLine(QString("[SSH] libssh connect failed → SFTP disabled: %1").arg(err));
                    }

                    watcher->deleteLater();
                });

        watcher->setFuture(QtConcurrent::run([this, p]() -> QPair<bool, QString> {
            QString e;
            const bool ok = m_ssh.connectProfile(p, &e);
            return qMakePair(ok, e);
        }));
    }

    // ---- 6) Update UI state ----
    if (m_connectBtn)    m_connectBtn->setEnabled(false);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(true);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Ready: %1").arg(shownTarget));
}


void MainWindow::onProfileDoubleClicked()
{
    onConnectClicked();
}

void MainWindow::onDisconnectClicked()
{
    m_ssh.disconnect();

    if (m_connectBtn)    m_connectBtn->setEnabled(true);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(false);

    if (m_statusLabel) m_statusLabel->setText("Disconnected.");
}

void MainWindow::onSendInput()
{
    // No interactive shell wired yet (ShellManager not implemented).
    const QString text = m_inputField ? m_inputField->text().trimmed() : QString();
    if (text.isEmpty()) return;

    appendTerminalLine(QString("> %1").arg(text));
    appendTerminalLine("[INFO] Shell not implemented yet; input not sent anywhere.");

    if (m_inputField) m_inputField->clear();
}

// ------------------------
// Drag-drop upload (will be connected once ShellManager exists)
// ------------------------

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

// ------------------------
// Download selection (needs ShellManager selection support)
// ------------------------

void MainWindow::downloadSelectionTriggered()
{
    appendTerminalLine("[DOWNLOAD] Shell not implemented yet (no selection source).");
}

// ------------------------
// PQ probe
// ------------------------

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

CpunkTermWidget* MainWindow::createTerm(const SshProfile &p, QWidget *parent)
{
    // IMPORTANT:
    // QTermWidget(int startnow, ...) starts the default shell if startnow != 0.
    // We MUST start with 0 so it doesn't launch a local bash before we configure ssh.
    auto *term = new CpunkTermWidget(0, parent);
    term->setHistorySize(2000);

    applyProfileToTerm(term, p);

    // ---------- Build SSH args ----------
    QStringList sshArgs;
    sshArgs << "-tt";
    if (p.pqDebug) sshArgs << "-vv";

    sshArgs << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    sshArgs << "-o" << "ConnectTimeout=5";
    sshArgs << "-o" << "ConnectionAttempts=1";

    // Avoid interactive host-key prompt inside embedded terminal
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

        // Disable muxing
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

    // ---------- Wrapper: exec ssh (no local shell fallback) ----------
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

    // Configure terminal program BEFORE startShellProgram()
    term->setShellProgram("/bin/bash");
    term->setArgs(QStringList() << "-lc" << cmd);

    term->setAutoClose(true);
    term->startShellProgram();

    // ---------- Styling ----------
    QTimer::singleShot(0,  term, [term]() { protectTermFromAppStyles(term); });
    QTimer::singleShot(50, term, [term]() { protectTermFromAppStyles(term); });

    const QString scheme = p.termColorScheme.isEmpty() ? "WhiteOnBlack" : p.termColorScheme;
    if (scheme == "WhiteOnBlack") {
        QTimer::singleShot(0,  term, [term]() { forceBlackBackground(term); });
        QTimer::singleShot(50, term, [term]() { forceBlackBackground(term); });
    }

    connect(term, &CpunkTermWidget::fileDropped,
            this, &MainWindow::onFileDropped);

    // Close tab/window on finished (your qtermwidget has finished() with no args)
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

    // Strong override vs global qApp stylesheet
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

    // Apply scheme FIRST (it updates palette internally)
    term->setColorScheme(scheme);

    // ✅ Now shield it from AppTheme::dark() overriding colors
    protectTermFromAppStyles(term);

    QFont f("Monospace");
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(p.termFontSize > 0 ? p.termFontSize : 11);
    term->setTerminalFont(f);

    term->setTerminalOpacity(1.0);

    // Optional: remove gray flash ONLY for WhiteOnBlack
    if (scheme == "WhiteOnBlack") {
        forceBlackBackground(term);        // forces black palette
        protectTermFromAppStyles(term);    // re-shield after forcing palette
    }
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

        // If user closes the terminal window, also disconnect libssh + update UI
        connect(w, &QObject::destroyed, this, [this]() {
            onDisconnectClicked();
        });

        w->show();
        w->raise();
        w->activateWindow();
        focusTerminalWindow(w, term);
        return;
    }

    // Tabbed window
    if (!m_tabbedShellWindow) {
        m_tabbedShellWindow = new QMainWindow();
        m_tabbedShellWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_tabbedShellWindow->setWindowTitle("PQ-SSH Tabs");

        m_tabWidget = new QTabWidget(m_tabbedShellWindow);
        m_tabWidget->setTabsClosable(true);
        m_tabbedShellWindow->setCentralWidget(m_tabWidget);

        // Close individual tabs; if last tab closes -> disconnect
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

        // If user closes the whole tab window, disconnect too
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

    // Optional: also reflect active tab in window title
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
