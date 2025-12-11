#include "MainWindow.h"
#include <QRegularExpression>
#include <QApplication>
#include <QWidget>
#include <QSplitter>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QStatusBar>
#include <QFontDatabase>
#include <QDateTime>
#include <QProcess>
#include <QCheckBox>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QFormLayout>   // ✅ for QFormLayout
#include <QSpinBox>      // ✅ for QSpinBox
#include <QTextCursor>
#include <QDebug>
#include <QInputDialog>
#include <libssh/libssh.h>

// ------------------------
// Dark theme stylesheet
// ------------------------
static QString darkStyleSheet()
{
    return R"(
        QMainWindow {
            background-color: #0f1115;
            color: #e0e0e0;
        }

        QWidget {
            background-color: #0f1115;
            color: #e0e0e0;
        }

        QLabel {
            color: #cfd8dc;
        }

        QListWidget {
            background-color: #141821;
            border: 1px solid #222;
            selection-background-color: #00c2ff;
            selection-color: #000000;
        }

        QListWidget::item {
            padding: 6px;
        }

        QLineEdit {
            background-color: #141821;
            border: 1px solid #333;
            padding: 6px;
            border-radius: 4px;
        }

        QPushButton {
            background-color: #1e2533;
            border: 1px solid #333;
            padding: 6px 12px;
            border-radius: 5px;
        }

        QPushButton:hover {
            background-color: #273046;
        }

        QPushButton:pressed {
            background-color: #00c2ff;
            color: #000000;
        }

        QPlainTextEdit {
            background-color: #0b0e14;
            border: 1px solid #222;
            padding: 8px;
            selection-background-color: #00c2ff;
            selection-color: #000000;
        }

        QMenuBar {
            background-color: #141821;
        }

        QMenuBar::item {
            background: transparent;
            padding: 6px 12px;
        }

        QMenuBar::item:selected {
            background-color: #00c2ff;
            color: #000000;
        }

        QMenu {
            background-color: #141821;
            border: 1px solid #222;
        }

        QMenu::item:selected {
            background-color: #00c2ff;
            color: #000000;
        }

        QStatusBar {
            background-color: #141821;
            color: #888;
        }

    )";
}

QString MainWindow::profilesConfigPath() const
{
    // Profiles live inside the project directory now:
    // pq-ssh/profiles/profiles.json

    QString baseDir = QCoreApplication::applicationDirPath();

    // If binary is in build/bin/, go up two levels to project root
    QDir dir(baseDir);
    dir.cdUp(); // bin -> build
    dir.cdUp(); // build -> pq-ssh

    QString profilesDir = dir.absolutePath() + "/profiles";

    if (!QDir().exists(profilesDir)) {
        QDir().mkpath(profilesDir);
    }

    return profilesDir + "/profiles.json";
}


void MainWindow::saveProfilesToDisk()
{
    QJsonArray arr;
    for (const auto &prof : m_profiles) {
        QJsonObject obj;
        obj["name"]     = prof.name;
        obj["user"]     = prof.user;
        obj["host"]     = prof.host;
        obj["port"]     = prof.port;
        obj["pq_debug"] = prof.pqDebug;
        arr.append(obj);
    }

    QJsonObject root;
    root["profiles"] = arr;

    QJsonDocument doc(root);

    QFile f(profilesConfigPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(doc.toJson(QJsonDocument::Indented));
        f.close();
    } else {
        appendTerminalLine("[ERROR] Could not write profiles.json");
        if (m_statusLabel) {
            m_statusLabel->setText("Failed to save profiles.json");
        }
    }
}



// ------------------------
// MainWindow
// ------------------------

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qApp->setStyleSheet(darkStyleSheet());

    setWindowTitle("CPUNK PQ-SSH");
    resize(1100, 700);

    setupUi();
    setupMenus();
    loadProfiles();
}

//Main window destructor
MainWindow::~MainWindow()
{
    // Stop shell worker if running
    if (m_shellWorker) {
        m_shellWorker->stopShell();
    }
    if (m_shellThread) {
        m_shellThread->quit();
        m_shellThread->wait();
        m_shellThread = nullptr;
    }

    // Clean up libssh session
    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }

    if (m_shellView) {
        m_shellView->close();
        delete m_shellView;
        m_shellView = nullptr;
    }

    // Old QProcess-based ssh cleanup (still safe to keep)
    if (m_sshProcess) {
        if (m_sshProcess->state() != QProcess::NotRunning) {
            m_sshProcess->kill();
            m_sshProcess->waitForFinished(2000);
        }
        delete m_sshProcess;
        m_sshProcess = nullptr;
    }
}




void MainWindow::setupUi()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    // ============================
    // Left: Profiles sidebar
    // ============================
    auto *profilesWidget = new QWidget(splitter);
    auto *profilesLayout = new QVBoxLayout(profilesWidget);
    profilesLayout->setContentsMargins(8, 8, 8, 8);
    profilesLayout->setSpacing(6);

    auto *profilesLabel = new QLabel("Profiles", profilesWidget);
    profilesLabel->setStyleSheet("font-weight: bold;");

    m_profileList = new QListWidget(profilesWidget);
    m_profileList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_editProfilesBtn = new QPushButton("Edit profiles…", profilesWidget);   // ✅ NEW
    m_editProfilesBtn->setToolTip("Open profiles.json in your default editor");

    profilesLayout->addWidget(profilesLabel);
    profilesLayout->addWidget(m_profileList, 1);
    profilesLayout->addWidget(m_editProfilesBtn, 0);                          // ✅ NEW

    profilesWidget->setLayout(profilesLayout);

    // ============================
    // Right: Session + terminal
    // ============================
    auto *rightWidget = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(6);

    // --- Top bar: host field + Connect button ---
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

    // --- Terminal area ---
    m_terminal = new QPlainTextEdit(rightWidget);
    m_terminal->setReadOnly(true);

    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(11);
    m_terminal->setFont(terminalFont);

    m_terminal->setPlaceholderText(
        "CPUNK PQ-SSH terminal\n"
        "SSH output will appear here.\n"
        "Type in the input field below and press Enter or Send."
    );

    // --- Input bar (send to ssh stdin) ---
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

    // --- Status label at bottom ---
    m_statusLabel = new QLabel("Ready.", rightWidget);
    m_statusLabel->setStyleSheet("color: gray;");

    // --- Bottom bar: status (left) + PQ indicator + debug toggle (right) ---
    // --- Bottom bar: status (left) + PQ indicator + debug toggle (right) ---
    auto *bottomBar = new QWidget(rightWidget);
    auto *bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);

    m_statusLabel = new QLabel("Ready.", bottomBar);
    m_statusLabel->setStyleSheet("color: gray;");

    m_pqStatusLabel = new QLabel("PQ: unknown", bottomBar);
    m_pqStatusLabel->setStyleSheet("color: #888; font-weight: bold;");

    m_pqDebugCheck = new QCheckBox("PQ debug", bottomBar);
    m_pqDebugCheck->setChecked(true);  // default: ON while we’re still testing
    m_pqDebugCheck->setToolTip("Show verbose SSH output (-vv) to confirm PQ KEX");

    bottomLayout->addWidget(m_statusLabel, 1);
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

    m_shellView = new TerminalView();                // ❗ no parent -> real top-level window
    m_shellView->setWindowTitle(QStringLiteral("PQ-SSH Shell"));
    m_shellView->setReadOnly(false);
    m_shellView->resize(900, 500);                  // give it a sane default size
    m_shellView->hide();                            // show when shell starts

    // Signals & slots
    connect(m_connectBtn, &QPushButton::clicked,
            this, &MainWindow::onConnectClicked);

    connect(m_disconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onDisconnectClicked);

    connect(m_profileList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onProfileDoubleClicked);

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &MainWindow::onProfileSelectionChanged);   // ✅ NEW

    connect(m_sendBtn, &QPushButton::clicked,
            this, &MainWindow::onSendInput);

    connect(m_inputField, &QLineEdit::returnPressed,
            this, &MainWindow::onSendInput);

    connect(m_editProfilesBtn, &QPushButton::clicked,
            this, &MainWindow::onEditProfilesClicked); 

    // Connect keystrokes to our onUserTyped slot
    connect(m_shellView, &TerminalView::bytesTyped,
            this, &MainWindow::onUserTyped);

}

void MainWindow::setupMenus()
{
    auto *fileMenu = menuBar()->addMenu("&File");
    // placeholders for later
    (void)fileMenu;

    auto *viewMenu = menuBar()->addMenu("&View");
    (void)viewMenu;

    auto *helpMenu = menuBar()->addMenu("&Help");
    (void)helpMenu;

    statusBar()->showMessage("CPUNK PQ-SSH prototype");
}


void MainWindow::appendTerminalLine(const QString &line)
{
    m_terminal->appendPlainText(line);
}


void MainWindow::updatePqStatusLabel(const QString &text, const QString &colorHex)
{
    if (!m_pqStatusLabel)
        return;

    m_pqStatusLabel->setText(text);
    m_pqStatusLabel->setStyleSheet(
        QString("color: %1; font-weight: bold;").arg(colorHex)
    );
}


bool MainWindow::establishSshSession(const QString &target)
{
    // Parse user@host
    QString user;
    QString host = target;
    const int atPos = target.indexOf('@');
    if (atPos != -1) {
        user = target.left(atPos);
        host = target.mid(atPos + 1);
    } else {
        user = qEnvironmentVariable("USER");
    }

    if (host.isEmpty()) {
        m_statusLabel->setText("No host specified.");
        return false;
    }

    // Clean up any old session
    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }

    ssh_session session = ssh_new();
    if (!session) {
        m_statusLabel->setText("Failed to create SSH session.");
        return false;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host.toUtf8().constData());
    if (!user.isEmpty()) {
        ssh_options_set(session, SSH_OPTIONS_USER, user.toUtf8().constData());
    }

    m_statusLabel->setText(QStringLiteral("Connecting to %1 ...").arg(target));
    qDebug() << "Connecting via libssh to" << user << "@" << host;

    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        QString err = QStringLiteral("SSH connect failed: %1")
                          .arg(QString::fromLocal8Bit(ssh_get_error(session)));
        m_statusLabel->setText(err);
        qDebug() << err;
        ssh_free(session);
        return false;
    }

    // Try public key auth first
    rc = ssh_userauth_publickey_auto(session, nullptr, nullptr);
    if (rc != SSH_AUTH_SUCCESS) {
        // Ask for password if keys fail
        QString prompt = QStringLiteral("Password for %1@%2")
                             .arg(user.isEmpty() ? QStringLiteral("(default user)") : user,
                                  host);
        bool ok = false;
        QString password = QInputDialog::getText(
            this,
            tr("SSH Password"),
            prompt,
            QLineEdit::Password,
            QString(),
            &ok
        );
        if (!ok) {
            m_statusLabel->setText("Authentication cancelled.");
            ssh_disconnect(session);
            ssh_free(session);
            return false;
        }

        rc = ssh_userauth_password(
            session,
            user.isEmpty() ? nullptr : user.toUtf8().constData(),
            password.toUtf8().constData()
        );
        if (rc != SSH_AUTH_SUCCESS) {
            QString err = QStringLiteral("SSH auth failed: %1")
                              .arg(QString::fromLocal8Bit(ssh_get_error(session)));
            m_statusLabel->setText(err);
            qDebug() << err;
            ssh_disconnect(session);
            ssh_free(session);
            return false;
        }
    }

    m_session = session;
    m_statusLabel->setText(QStringLiteral("Connected to %1").arg(target));
    qDebug() << "SSH session established to" << target;
    return true;
}



void MainWindow::onConnectClicked()
{
    const QString target = m_hostField->text().trimmed();
    qDebug() << "onConnectClicked() called, target:" << target;

    if (target.isEmpty()) {
        m_statusLabel->setText("No host specified.");
        qDebug() << "Connect aborted: no host specified.";
        return;
    }

    // You can keep this old guard if you still use m_sshProcess elsewhere.
    if (m_sshProcess && m_sshProcess->state() != QProcess::NotRunning) {
        m_statusLabel->setText("SSH session already running. Close it before starting a new one.");
        qDebug() << "Connect aborted: SSH session already running. State:"
                 << m_sshProcess->state();
        return;
    }

    // 1) Establish libssh session
    if (!establishSshSession(target)) {
        qDebug() << "establishSshSession() failed for" << target;
        return;
    }

    // 2) Start PTY + interactive shell
    qDebug() << "Starting interactive shell to" << target;
    startInteractiveShell();
    qDebug() << "startInteractiveShell() called";
}



void MainWindow::onProfileSelectionChanged(int row)
{
    if (row < 0 || row >= m_profiles.size())
        return;

    const SshProfile &p = m_profiles[row];
    m_hostField->setText(QString("%1@%2").arg(p.user, p.host));
    if (m_pqDebugCheck)
        m_pqDebugCheck->setChecked(p.pqDebug);
}

void MainWindow::onProfileDoubleClicked()
{
    int row = m_profileList->currentRow();
    if (row < 0 || row >= m_profiles.size())
        return;

    const SshProfile &p = m_profiles[row];
    m_hostField->setText(QString("%1@%2").arg(p.user, p.host));
    if (m_pqDebugCheck)
        m_pqDebugCheck->setChecked(p.pqDebug);

    onConnectClicked();
}




void MainWindow::onSendInput()
{
    if (!m_sshProcess || m_sshProcess->state() == QProcess::NotRunning) {
        m_statusLabel->setText("No active SSH session.");
        return;
    }

    const QString text = m_inputField->text();
    if (text.isEmpty())
        return;

    appendTerminalLine(QString("> %1").arg(text));

    QByteArray data = text.toUtf8();
    data.append('\n');
    m_sshProcess->write(data);   // ✅ this is enough

    m_inputField->clear();
}

void MainWindow::onDisconnectClicked()
{
    if (!m_sshProcess || m_sshProcess->state() == QProcess::NotRunning) {
        m_statusLabel->setText("No active SSH session to disconnect.");
        return;
    }

    m_statusLabel->setText("Disconnecting...");
    appendTerminalLine("[SSH] Disconnect requested by user.");

    // Ask ssh nicely first
    m_sshProcess->terminate();

    // If it doesn't die quickly, we can force-kill later
    if (!m_sshProcess->waitForFinished(1500)) {
        m_sshProcess->kill();
        m_sshProcess->waitForFinished(1500);
    }

    m_connectBtn->setEnabled(true);
    m_disconnectBtn->setEnabled(false);
}

void MainWindow::startSshProcess(const QString &target)
{
    if (!m_sshProcess) {
        m_sshProcess = new QProcess(this);

        m_sshProcess->setProcessChannelMode(QProcess::MergedChannels);

        connect(m_sshProcess, &QProcess::readyRead,
                this, &MainWindow::handleSshReadyRead);

        connect(m_sshProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &MainWindow::handleSshFinished);

        connect(m_sshProcess,
                &QProcess::errorOccurred,
                this, &MainWindow::handleSshError);
    }

    // Clear old output
    m_terminal->clear();
    m_connectBtn->setEnabled(false);
    m_disconnectBtn->setEnabled(false);

    m_pqActive = false;

    const bool debugEnabled = (m_pqDebugCheck && m_pqDebugCheck->isChecked());

    if (debugEnabled) {
        updatePqStatusLabel("PQ: trying…", "#ffca28");              // yellow
    } else {
        updatePqStatusLabel("PQ: requested (no debug)", "#90caf9"); // blue-ish
    }

    QString program = "ssh";

    QStringList args;
    args << "-tt";

    if (debugEnabled) {           // ✅ reuse the same variable here
        args << "-vv";
    }

    args << "-o" << "KexAlgorithms=+sntrup761x25519-sha512@openssh.com";
    args << target;

    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODate);
    appendTerminalLine(QString("[%1] Starting ssh -> %2").arg(ts, target));

    m_sshProcess->start(program, args);

    if (!m_sshProcess->waitForStarted(3000)) {
        appendTerminalLine("[ERROR] Failed to start ssh process.");
        m_statusLabel->setText("Failed to start ssh process.");
        m_connectBtn->setEnabled(true);
        m_disconnectBtn->setEnabled(false);
        return;
    }

    m_connectBtn->setEnabled(false);
    m_disconnectBtn->setEnabled(true);

    m_statusLabel->setText(QString("SSH session running to %1").arg(target));
}

void MainWindow::handleSshReadyRead()
{
    if (!m_sshProcess)
        return;

    const QByteArray data = m_sshProcess->readAll();
    if (data.isEmpty())
        return;

    const QString text = QString::fromUtf8(data);
    appendTerminalLine(text);

    // Heuristic: detect when PQ KEX is actually negotiated
    // (this line appears with -vv / -vvv enabled, but can appear in other cases too)
    if (text.contains("kex: algorithm: sntrup761x25519-sha512@openssh.com")) {
        m_pqActive = true;
        updatePqStatusLabel("PQ: ACTIVE", "#4caf50");  // green
    }

    // Detect failures / fallback
    if (text.contains("Unsupported KEX algorithm") ||
        text.contains("no matching key exchange method")) {
        m_pqActive = false;
        updatePqStatusLabel("PQ: OFF", "#ff5252");     // red
    }
}

void MainWindow::handleSshFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    appendTerminalLine(QString("[SSH] Process finished with exit code %1").arg(exitCode));
    m_statusLabel->setText("SSH session finished.");

    // ✅ Restore UI state
    m_connectBtn->setEnabled(true);
    m_disconnectBtn->setEnabled(false);

    if (!m_pqActive) {
        // Session ended without seeing PQ KEX line → unknown/fallback
        updatePqStatusLabel("PQ: unknown", "#888888");
    }

}

void MainWindow::handleSshError(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:
        msg = "Failed to start ssh (not found or not executable).";
        break;
    case QProcess::Crashed:
        msg = "ssh process crashed.";
        break;
    case QProcess::Timedout:
        msg = "ssh process timeout.";
        break;
    case QProcess::WriteError:
        msg = "ssh write error.";
        break;
    case QProcess::ReadError:
        msg = "ssh read error.";
        break;
    case QProcess::UnknownError:
    default:
        msg = "Unknown ssh process error.";
        break;
    }

    appendTerminalLine("[ERROR] " + msg);
    m_statusLabel->setText(msg);

    // ✅ Ensure buttons restore
    m_connectBtn->setEnabled(true);
    m_disconnectBtn->setEnabled(false);
    m_pqActive = false;
    updatePqStatusLabel("PQ: OFF", "#ff5252");
}


void MainWindow::createDefaultProfiles()
{
    m_profiles.clear();

    const QString user = qEnvironmentVariable("USER", "user");

    SshProfile p;
    p.name    = "Localhost";
    p.user    = user;
    p.host    = "localhost";
    p.port    = 22;
    p.pqDebug = true;

    m_profiles.push_back(p);

    // Also persist to disk so profiles/profiles.json exists
    saveProfilesToDisk();
}


void MainWindow::onEditProfilesClicked()
{
    showProfilesEditor();
}



void MainWindow::loadProfiles()
{
    m_profiles.clear();
    m_profileList->clear();

    const QString path = profilesConfigPath();
    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QByteArray data = f.readAll();
        f.close();

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject root = doc.object();
            QJsonArray arr = root["profiles"].toArray();

            for (const QJsonValue &val : arr) {
                if (!val.isObject())
                    continue;
                QJsonObject obj = val.toObject();

                SshProfile p;
                p.name    = obj["name"].toString();
                p.user    = obj["user"].toString();
                p.host    = obj["host"].toString();
                p.port    = obj["port"].toInt(22);
                p.pqDebug = obj["pq_debug"].toBool(true);

                if (p.user.isEmpty() || p.host.isEmpty())
                    continue; // skip invalid entries

                if (p.name.isEmpty())
                    p.name = QString("%1@%2").arg(p.user, p.host);

                m_profiles.push_back(p);
            }
        }
    }

    // If we ended up with nothing, create a default file
    if (m_profiles.isEmpty()) {
        createDefaultProfiles();
    }

    // Populate the list widget
    m_profileList->clear();
    for (const auto &p : m_profiles) {
        m_profileList->addItem(p.name);
    }

    if (!m_profiles.isEmpty()) {
        m_profileList->setCurrentRow(0);
        const SshProfile &p = m_profiles[0];
        m_hostField->setText(QString("%1@%2").arg(p.user, p.host));
        if (m_pqDebugCheck)
            m_pqDebugCheck->setChecked(p.pqDebug);
    }
}


void MainWindow::showProfilesEditor()
{
    // Work on a local copy; we'll only commit if user presses Save.
    QVector<SshProfile> profiles = m_profiles;
    if (profiles.isEmpty()) {
        createDefaultProfiles();
        profiles = m_profiles;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Manage SSH Profiles");
    dlg.resize(750, 500);

    auto *mainLayout = new QHBoxLayout(&dlg);

    // ---------- Left: profile list + Add/Delete ----------
    auto *leftWidget = new QWidget(&dlg);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);

    auto *listLabel = new QLabel("Profiles", leftWidget);
    listLabel->setStyleSheet("font-weight: bold;");

    auto *list = new QListWidget(leftWidget);
    list->setSelectionMode(QAbstractItemView::SingleSelection);

    for (const auto &p : profiles) {
        list->addItem(p.name);
    }

    auto *buttonsRow = new QWidget(leftWidget);
    auto *buttonsLayout = new QHBoxLayout(buttonsRow);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(6);

    auto *addBtn = new QPushButton("Add", buttonsRow);
    auto *delBtn = new QPushButton("Delete", buttonsRow);

    buttonsLayout->addWidget(addBtn);
    buttonsLayout->addWidget(delBtn);
    buttonsRow->setLayout(buttonsLayout);

    leftLayout->addWidget(listLabel);
    leftLayout->addWidget(list, 1);
    leftLayout->addWidget(buttonsRow, 0);
    leftWidget->setLayout(leftLayout);

    // ---------- Right: profile details form ----------
    auto *rightWidget = new QWidget(&dlg);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    auto *nameEdit   = new QLineEdit(rightWidget);
    auto *userEdit   = new QLineEdit(rightWidget);
    auto *hostEdit   = new QLineEdit(rightWidget);
    auto *portSpin   = new QSpinBox(rightWidget);
    portSpin->setRange(1, 65535);
    portSpin->setValue(22);

    auto *pqDebugCheck = new QCheckBox("Enable PQ debug (-vv)", rightWidget);

    form->addRow("Name:", nameEdit);
    form->addRow("User:", userEdit);
    form->addRow("Host:", hostEdit);
    form->addRow("Port:", portSpin);
    form->addRow("", pqDebugCheck);

    rightLayout->addLayout(form);

    auto *buttonsBox = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel,
        rightWidget
    );
    rightLayout->addWidget(buttonsBox);

    rightWidget->setLayout(rightLayout);

    // Put both sides into the main layout
    mainLayout->addWidget(leftWidget, 1);
    mainLayout->addWidget(rightWidget, 2);

    // ---------- Selection and editing logic ----------
    int currentRow = -1;

    auto loadProfileToForm = [&](int row) {
        if (row < 0 || row >= profiles.size()) {
            nameEdit->clear();
            userEdit->clear();
            hostEdit->clear();
            portSpin->setValue(22);
            pqDebugCheck->setChecked(true);
            return;
        }
        const SshProfile &p = profiles[row];
        nameEdit->setText(p.name);
        userEdit->setText(p.user);
        hostEdit->setText(p.host);
        portSpin->setValue(p.port);
        pqDebugCheck->setChecked(p.pqDebug);
    };

    auto syncFormToCurrent = [&]() {
        if (currentRow < 0 || currentRow >= profiles.size())
            return;
        SshProfile &p = profiles[currentRow];
        p.name    = nameEdit->text().trimmed();
        p.user    = userEdit->text().trimmed();
        p.host    = hostEdit->text().trimmed();
        p.port    = portSpin->value();
        p.pqDebug = pqDebugCheck->isChecked();
        if (p.name.isEmpty()) {
            p.name = QString("%1@%2").arg(p.user, p.host);
        }
        if (QListWidgetItem *item = list->item(currentRow)) {
            item->setText(p.name);
        }
    };

    // Initial selection
    if (!profiles.isEmpty()) {
        currentRow = 0;
        list->setCurrentRow(0);
        loadProfileToForm(0);
    }

    QObject::connect(list, &QListWidget::currentRowChanged,
                     &dlg, [&](int row) {
        // Save changes from previous selection
        syncFormToCurrent();
        currentRow = row;
        loadProfileToForm(row);
    });

    QObject::connect(nameEdit, &QLineEdit::textChanged,
                     &dlg, [&](const QString &text) {
        if (currentRow < 0 || currentRow >= profiles.size())
            return;
        profiles[currentRow].name = text;
        if (QListWidgetItem *item = list->item(currentRow)) {
        item->setText(text.isEmpty() ? QString("%1@%2").arg(userEdit->text(), hostEdit->text()) : text);
        }
    });

    QObject::connect(addBtn, &QPushButton::clicked,
                     &dlg, [&]() {
        SshProfile p;
        p.user    = qEnvironmentVariable("USER", "user");
        p.host    = "localhost";
        p.port    = 22;
        p.pqDebug = true;
        p.name    = QString("%1@%2").arg(p.user, p.host);

        profiles.push_back(p);
        list->addItem(p.name);

        int row = profiles.size() - 1;
        list->setCurrentRow(row);
    });

    QObject::connect(delBtn, &QPushButton::clicked,
                     &dlg, [&]() {
        int row = list->currentRow();
        if (row < 0 || row >= profiles.size())
            return;

        profiles.remove(row);
        delete list->takeItem(row);

        if (profiles.isEmpty()) {
            currentRow = -1;
            loadProfileToForm(-1);
        } else {
            int newRow = qMin(row, profiles.size() - 1);
            currentRow = newRow;
            list->setCurrentRow(newRow);
            loadProfileToForm(newRow);
        }
    });

    QObject::connect(buttonsBox, &QDialogButtonBox::rejected,
                     &dlg, &QDialog::reject);

    QObject::connect(buttonsBox, &QDialogButtonBox::accepted,
                     &dlg, [&]() {
        syncFormToCurrent();

        // Basic validation: no empty user/host
        for (const auto &p : profiles) {
            if (p.user.trimmed().isEmpty() || p.host.trimmed().isEmpty()) {
                QMessageBox::warning(
                    &dlg,
                    "Invalid profile",
                    "Each profile must have non-empty user and host."
                );
                return;
            }
        }

        // Commit changes
        m_profiles = profiles;
        saveProfilesToDisk();
        loadProfiles();   // refresh sidebar with new data

        dlg.accept();
        m_statusLabel->setText("Profiles updated.");
        appendTerminalLine("[INFO] Profiles updated via profile manager.");
    });

    dlg.exec();
}



void MainWindow::startInteractiveShell()
{
    if (!m_session) {
        m_statusLabel->setText("No SSH session (m_session is null).");
        qDebug() << "startInteractiveShell() aborted: m_session is null";
        return;
    }

    if (m_shellThread) {
        qDebug() << "startInteractiveShell(): shell already running";
        return;
    }




    if (m_shellView) {
        m_shellView->clear();

        // Position it near the center of the main window the first time
        if (!m_shellView->isVisible()) {
            QRect mw = this->geometry();
            int x = mw.center().x() - m_shellView->width() / 2;
            int y = mw.center().y() - m_shellView->height() / 2;
            m_shellView->move(x, y);
        }

    m_shellView->show();
    m_shellView->raise();
    m_shellView->activateWindow();
}

    m_shellThread = new QThread(this);
    m_shellWorker = new SshShellWorker(m_session, 120, 32); // cols/rows

    m_shellWorker->moveToThread(m_shellThread);

    connect(m_shellThread, &QThread::started,
            m_shellWorker, &SshShellWorker::startShell);

    connect(m_shellWorker, &SshShellWorker::outputReady,
            this, &MainWindow::handleShellOutput);

    connect(m_shellWorker, &SshShellWorker::shellClosed,
            this, &MainWindow::handleShellClosed);

    connect(m_shellWorker, &SshShellWorker::shellClosed,
            m_shellThread, &QThread::quit);

    connect(m_shellThread, &QThread::finished,
            m_shellWorker, &QObject::deleteLater);
    connect(m_shellThread, &QThread::finished,
            m_shellThread, &QObject::deleteLater);

    m_shellThread->start();
}


void MainWindow::onUserTyped(const QByteArray &data)
{
    if (!m_shellWorker)
        return;

    m_shellWorker->sendInput(data);
}


void MainWindow::handleShellOutput(const QByteArray &data)
{
    if (!m_shellView)
        return;

    if (!m_shellView->isVisible())
        m_shellView->show();

    QString text = QString::fromLocal8Bit(data);

    // Remove most CSI (cursor/colour) sequences: ESC [ ... letter
    static QRegularExpression csiRe(QStringLiteral("\x1B\\[[0-9;?]*[ -/]*[@-~]"));
    text.remove(csiRe);

    // Remove OSC sequences (like changing window title): ESC ] ... BEL
    static QRegularExpression oscRe(QStringLiteral("\x1B\\][^\\a]*\\a"));
    text.remove(oscRe);

    // Remove stray BEL characters (beeps) if any remain
    text.remove(QChar('\x07'));

    m_shellView->moveCursor(QTextCursor::End);
    m_shellView->insertPlainText(text);
    m_shellView->moveCursor(QTextCursor::End);
}


void MainWindow::handleShellClosed(const QString &reason)
{
    if (m_shellView) {
        m_shellView->appendPlainText(
            QStringLiteral("\n[Shell closed] ") + reason + QStringLiteral("\n")
        );
    }

    m_shellThread = nullptr;
    m_shellWorker = nullptr;
}

