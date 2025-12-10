#include "MainWindow.h"

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
    loadDummyProfiles();
}

MainWindow::~MainWindow()
{
    if (m_sshProcess) {
        m_sshProcess->kill();
        m_sshProcess->waitForFinished(2000);
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

    profilesLayout->addWidget(profilesLabel);
    profilesLayout->addWidget(m_profileList);
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

    rightLayout->addWidget(topBar);
    rightLayout->addWidget(m_terminal, 1);
    rightLayout->addWidget(inputBar);
    rightLayout->addWidget(m_statusLabel);
    rightWidget->setLayout(rightLayout);

    splitter->addWidget(profilesWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // Signals & slots
    connect(m_connectBtn, &QPushButton::clicked,
            this, &MainWindow::onConnectClicked);

    connect(m_profileList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onProfileDoubleClicked);

    connect(m_sendBtn, &QPushButton::clicked,
            this, &MainWindow::onSendInput);

    connect(m_inputField, &QLineEdit::returnPressed,
            this, &MainWindow::onSendInput);

    connect(m_disconnectBtn, &QPushButton::clicked,
            this, &MainWindow::onDisconnectClicked);
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

void MainWindow::loadDummyProfiles()
{
    QStringList demoProfiles = {
        "timo@localhost",
        "root@my-dht-node",
        "cpunk@remote-gateway"
    };

    for (const auto &name : demoProfiles) {
        m_profileList->addItem(name);
    }

    if (m_profileList->count() > 0) {
        m_profileList->setCurrentRow(0);
        m_hostField->setText(m_profileList->currentItem()->text());
    }
}

void MainWindow::appendTerminalLine(const QString &line)
{
    m_terminal->appendPlainText(line);
}

void MainWindow::onConnectClicked()
{
    const QString target = m_hostField->text().trimmed();
    if (target.isEmpty()) {
        m_statusLabel->setText("No host specified.");
        return;
    }

    if (m_sshProcess && m_sshProcess->state() != QProcess::NotRunning) {
        m_statusLabel->setText("SSH session already running. Close it before starting a new one.");
        return;
    }

    startSshProcess(target);
}

void MainWindow::onProfileDoubleClicked()
{
    auto *item = m_profileList->currentItem();
    if (!item) return;

    m_hostField->setText(item->text());
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
    m_disconnectBtn->setEnabled(false);  // will re-enable once started

    QString program = "ssh";

    QStringList args;
    args << "-tt";  // force TTY allocation (better for interactive)

    // Request hybrid PQ KEX, but *add* it to existing set (with '+'),
    // so ssh can still negotiate something else if the server doesn't support it.
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

    // ✅ Session is starting – adjust UI
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

    appendTerminalLine(QString::fromUtf8(data));
}

void MainWindow::handleSshFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    appendTerminalLine(QString("[SSH] Process finished with exit code %1").arg(exitCode));
    m_statusLabel->setText("SSH session finished.");

    // ✅ Restore UI state
    m_connectBtn->setEnabled(true);
    m_disconnectBtn->setEnabled(false);
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
}
