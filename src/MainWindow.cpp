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
#include <QDialog>
#include <QDebug>

#include "AppTheme.h"
#include "ProfileStore.h"
#include "ProfilesEditorDialog.h"
#include "SshClient.h"
#include <QTabWidget>
#include <QFont>
#include "CpunkTermWidget.h"

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

    if (m_hostField)
        m_hostField->setText(QString("%1@%2").arg(p.user, p.host));

    if (m_pqDebugCheck)
        m_pqDebugCheck->setChecked(p.pqDebug);
}

void MainWindow::onProfileDoubleClicked()
{
    onConnectClicked();
}

void MainWindow::onConnectClicked()
{
    const QString target = m_hostField ? m_hostField->text().trimmed() : QString();
    if (target.isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText("No host specified.");
        return;
    }

    const int row = m_profileList ? m_profileList->currentRow() : -1;
    if (row < 0 || row >= m_profiles.size()) {
        if (m_statusLabel) m_statusLabel->setText("No profile selected.");
        return;
    }

    if (m_statusLabel)
        m_statusLabel->setText(QString("Connecting to %1 ...").arg(target));

    // ✅ Password-based system right now -> do NOT attempt public-key libssh
    appendTerminalLine("[SSH] Password-based login in use. SFTP (libssh) disabled for now.");

    // PQ probe (still ok)
    const bool pqOk = probePqSupport(target);
    updatePqStatusLabel(pqOk ? "PQ: ACTIVE" : "PQ: OFF", pqOk ? "#4caf50" : "#ff5252");

    const bool newWindow = (m_openInNewWindowCheck && m_openInNewWindowCheck->isChecked());
    openShellForProfile(m_profiles[row], target, newWindow);

    if (m_connectBtn)    m_connectBtn->setEnabled(false);
    if (m_disconnectBtn) m_disconnectBtn->setEnabled(true);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Ready: %1").arg(target));
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
    auto *term = new CpunkTermWidget(2000, parent); // history lines
    applyProfileToTerm(term, p);

    // Start ssh inside the embedded terminal (password prompts will appear here)
    term->setShellProgram("ssh");

    QStringList args;
    args << "-tt";
    if (p.pqDebug) args << "-vv";
    args << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    args << p.user + "@" + p.host;

    term->setArgs(args);
    term->startShellProgram();

    // Drag-drop from local -> app handler
    connect(term, &CpunkTermWidget::fileDropped,
            this, &MainWindow::onFileDropped);

    return term;
}

void MainWindow::applyProfileToTerm(CpunkTermWidget *term, const SshProfile &p)
{
    if (!term) return;

    // Color scheme (must match a scheme installed for qtermwidget)
    if (!p.termColorScheme.isEmpty())
        term->setColorScheme(p.termColorScheme);

    // Set a fixed font + size (don’t rely on terminalFont())
    QFont f("Monospace");
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(p.termFontSize > 0 ? p.termFontSize : 11);
    term->setTerminalFont(f);
}

void MainWindow::openShellForProfile(const SshProfile &p, const QString &target, bool newWindow)
{
    Q_UNUSED(target);

    if (newWindow) {
        auto *w = new QMainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose, true);
        w->setWindowTitle(QString("PQ-SSH: %1").arg(p.name));
        w->resize(p.termWidth > 0 ? p.termWidth : 900,
                  p.termHeight > 0 ? p.termHeight : 500);

        auto *term = createTerm(p, w);
        w->setCentralWidget(term);

        w->show();
        w->raise();
        w->activateWindow();
        return;
    }

    // Tabbed window
    if (!m_tabbedShellWindow) {
        m_tabbedShellWindow = new QMainWindow();
        m_tabbedShellWindow->setAttribute(Qt::WA_DeleteOnClose, true);
        m_tabbedShellWindow->setWindowTitle("PQ-SSH Tabs");

        m_tabWidget = new QTabWidget(m_tabbedShellWindow);
        m_tabbedShellWindow->setCentralWidget(m_tabWidget);

        // if user closes the tab window, clear pointers
        connect(m_tabbedShellWindow, &QObject::destroyed, this, [this]() {
            m_tabbedShellWindow = nullptr;
            m_tabWidget = nullptr;
        });

        m_tabbedShellWindow->resize(1000, 650);
    }

    auto *term = createTerm(p, m_tabWidget);
    const int idx = m_tabWidget->addTab(term, p.name);
    m_tabWidget->setCurrentIndex(idx);

    m_tabbedShellWindow->show();
    m_tabbedShellWindow->raise();
    m_tabbedShellWindow->activateWindow();
}
