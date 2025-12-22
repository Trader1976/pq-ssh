#include "FleetWindow.h"

#include <QCryptographicHash>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>
#include <QDialog>
#include <QTextEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QSettings>

#include "AuditLogger.h"

static QString normalizedGroup(const QString& g)
{
    const QString s = g.trimmed();
    return s.isEmpty() ? QStringLiteral("Ungrouped") : s;
}

static bool isHeaderItem(const QListWidgetItem* it)
{
    return it && (it->data(Qt::UserRole).toInt() == -1);
}

FleetWindow::FleetWindow(const QVector<SshProfile>& profiles, QWidget* parent)
    : QMainWindow(parent)
    , m_profiles(profiles)
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowTitle("CPUNK PQ-SSH — Fleet Jobs");
    resize(1200, 720);

    m_exec = new FleetExecutor(this);

    buildUi();
    rebuildTargetsList();

    connect(m_exec, &FleetExecutor::jobStarted,  this, &FleetWindow::onJobStarted);
    connect(m_exec, &FleetExecutor::jobProgress, this, &FleetWindow::onJobProgress);
    connect(m_exec, &FleetExecutor::jobFinished, this, &FleetWindow::onJobFinished);
}

FleetWindow::~FleetWindow() = default;

void FleetWindow::buildUi()
{
    auto* root = new QWidget(this);
    setCentralWidget(root);

    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    // Top: filter + selection helpers
    auto* topBar = new QWidget(root);
    auto* topL = new QHBoxLayout(topBar);
    topL->setContentsMargins(0, 0, 0, 0);
    topL->setSpacing(8);

    m_filterEdit = new QLineEdit(topBar);
    m_filterEdit->setPlaceholderText("Filter targets (name, host, group) ...");

    m_selectAllBtn = new QPushButton("Select all", topBar);
    m_selectNoneBtn = new QPushButton("Select none", topBar);

    topL->addWidget(new QLabel("Targets:", topBar));
    topL->addWidget(m_filterEdit, 1);
    topL->addWidget(m_selectAllBtn);
    topL->addWidget(m_selectNoneBtn);

    // Split: left targets, right actions/results
    auto* split = new QSplitter(Qt::Horizontal, root);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    // Left: target list
    auto* left = new QWidget(split);
    auto* leftL = new QVBoxLayout(left);
    leftL->setContentsMargins(0, 0, 0, 0);
    leftL->setSpacing(6);

    m_targetsList = new QListWidget(left);
    m_targetsList->setSelectionMode(QAbstractItemView::NoSelection);

    leftL->addWidget(m_targetsList, 1);

    // Right: actions + results/log
    auto* right = new QWidget(split);
    auto* rightL = new QVBoxLayout(right);
    rightL->setContentsMargins(0, 0, 0, 0);
    rightL->setSpacing(8);

    // Action box
    auto* actionBox = new QGroupBox("Fleet action", right);
    auto* actionL = new QVBoxLayout(actionBox);

    auto* aTop = new QWidget(actionBox);
    auto* aTopL = new QHBoxLayout(aTop);
    aTopL->setContentsMargins(0, 0, 0, 0);
    aTopL->setSpacing(8);

    m_actionCombo = new QComboBox(aTop);
    m_actionCombo->addItem("Run command", (int)FleetActionType::RunCommand);
    m_actionCombo->addItem("Check service (systemd)", (int)FleetActionType::CheckService);
    m_actionCombo->addItem("Restart service (systemd)", (int)FleetActionType::RestartService);

    m_concurrencySpin = new QSpinBox(aTop);
    m_concurrencySpin->setRange(1, 32);
    m_concurrencySpin->setValue(4);
    m_concurrencySpin->setToolTip("Max parallel targets");

    // NEW: Timeout (seconds)
    m_timeoutSpin = new QSpinBox(aTop);
    m_timeoutSpin->setRange(1, 3600);
    m_timeoutSpin->setValue(90);
    m_timeoutSpin->setSuffix(" s");
    m_timeoutSpin->setToolTip("Per-target command timeout (seconds).");
    aTopL->addWidget(new QLabel("Timeout:", aTop));
    aTopL->addWidget(m_timeoutSpin);

    // NEW: Audit command logging mode (per-user setting)
    m_cmdAuditCombo = new QComboBox(aTop);
    m_cmdAuditCombo->addItem("None", 0);              // log nothing about command
    m_cmdAuditCombo->addItem("Safe (head + hash)", 1);// cmd_head + cmd_hash
    m_cmdAuditCombo->addItem("Full (risky)", 2);      // cmd_full (opt-in)
    m_cmdAuditCombo->setToolTip(
        "Controls what gets written to Audit Logs for Fleet commands.\n"
        "None: no command data\n"
        "Safe: logs cmd_head + cmd_hash (recommended)\n"
        "Full: logs full command text (may leak secrets!)"
    );
    // Load initial value from settings
    {
        QSettings s;
        const int mode = qBound(0, s.value("audit/commandLogMode", 1).toInt(), 2);
        for (int i = 0; i < m_cmdAuditCombo->count(); ++i) {
            if (m_cmdAuditCombo->itemData(i).toInt() == mode) {
                m_cmdAuditCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    // Save on change (+ warn if Full)
    connect(m_cmdAuditCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {

        if (!m_cmdAuditCombo) return;

        const int mode = qBound(0, m_cmdAuditCombo->itemData(idx).toInt(), 2);

        QSettings s;
        s.setValue("audit/commandLogMode", mode);

        if (mode == 2) {
            // One-time warning
            if (!s.value("audit/warnedFullCmdLog", false).toBool()) {
                s.setValue("audit/warnedFullCmdLog", true);
                QMessageBox::warning(
                    this,
                    "Audit logging: Full commands",
                    "You enabled FULL command logging in audit logs.\n\n"
                    "This can leak secrets (tokens, passwords, private paths) into audit files.\n"
                    "Recommended setting is: Safe (head + hash)."
                );
            }
        }
    });

    aTopL->addWidget(new QLabel("Action:", aTop));
    aTopL->addWidget(m_actionCombo, 1);

    aTopL->addWidget(new QLabel("Concurrency:", aTop));
    aTopL->addWidget(m_concurrencySpin);

    aTopL->addWidget(new QLabel("Timeout:", aTop));
    aTopL->addWidget(m_timeoutSpin);

    aTopL->addWidget(new QLabel("Audit cmd log:", aTop));
    aTopL->addWidget(m_cmdAuditCombo);

    // Action stacked pages
    m_actionStack = new QStackedWidget(actionBox);

    // Page 0: RunCommand
    {
        auto* page = new QWidget(m_actionStack);
        auto* l = new QFormLayout(page);
        l->setContentsMargins(0, 0, 0, 0);

        m_cmdEdit = new QLineEdit(page);
        m_cmdEdit->setPlaceholderText("e.g. uname -a");
        l->addRow("Command:", m_cmdEdit);

        m_actionStack->addWidget(page);
    }

    // Page 1: CheckService
    {
        auto* page = new QWidget(m_actionStack);
        auto* l = new QFormLayout(page);
        l->setContentsMargins(0, 0, 0, 0);

        m_serviceEdit = new QLineEdit(page);
        m_serviceEdit->setPlaceholderText("e.g. nginx");
        l->addRow("Service:", m_serviceEdit);

        m_actionStack->addWidget(page);
    }

    // Page 2: RestartService
    {
        auto* page = new QWidget(m_actionStack);
        auto* l = new QFormLayout(page);
        l->setContentsMargins(0, 0, 0, 0);

        // Reuse same service edit instance? No (cleaner UI): create a new one
        auto* svc = new QLineEdit(page);
        svc->setPlaceholderText("e.g. nginx");
        // We'll mirror its text to m_serviceEdit for simplicity
        connect(svc, &QLineEdit::textChanged, this, [this](const QString& t) {
            if (m_serviceEdit) m_serviceEdit->setText(t);
        });
        connect(m_serviceEdit, &QLineEdit::textChanged, this, [svc](const QString& t) {
            if (svc->text() != t) svc->setText(t);
        });

        m_confirmDanger = new QCheckBox("I understand this is disruptive (restart).", page);
        l->addRow("Service:", svc);
        l->addRow("", m_confirmDanger);

        m_actionStack->addWidget(page);
    }

    actionL->addWidget(aTop);
    actionL->addWidget(m_actionStack);

    // Run bar
    auto* runBar = new QWidget(right);
    auto* runL = new QHBoxLayout(runBar);
    runL->setContentsMargins(0, 0, 0, 0);
    runL->setSpacing(8);

    m_runBtn = new QPushButton("Run fleet job", runBar);
    m_cancelBtn = new QPushButton("Cancel", runBar);
    m_cancelBtn->setEnabled(false);

    m_statusLabel = new QLabel("Ready.", runBar);
    m_statusLabel->setStyleSheet("color:#888;");

    runL->addWidget(m_runBtn);
    runL->addWidget(m_cancelBtn);
    runL->addWidget(m_statusLabel, 1);

    // Results table
    m_resultsTable = new QTableWidget(right);
    m_resultsTable->setColumnCount(7);
    m_resultsTable->setHorizontalHeaderLabels({
        "Profile", "Group", "Target", "Status", "Duration", "Stdout (preview)", "Error (preview)"
    });
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->setShowGrid(false);
    m_resultsTable->setAlternatingRowColors(true);
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_resultsTable->verticalHeader()->setVisible(false);

    // Log pane
    m_log = new QPlainTextEdit(right);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText("Fleet log…");

    auto* rightSplit = new QSplitter(Qt::Vertical, right);
    rightSplit->setChildrenCollapsible(false);
    rightSplit->addWidget(m_resultsTable);
    rightSplit->addWidget(m_log);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 1);

    rightL->addWidget(actionBox, 0);
    rightL->addWidget(runBar, 0);
    rightL->addWidget(rightSplit, 1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({320, 880});

    outer->addWidget(topBar, 0);
    outer->addWidget(split, 1);

    // Wiring
    connect(m_filterEdit, &QLineEdit::textChanged, this, &FleetWindow::onFilterChanged);
    connect(m_selectAllBtn, &QPushButton::clicked, this, &FleetWindow::onSelectAll);
    connect(m_selectNoneBtn, &QPushButton::clicked, this, &FleetWindow::onSelectNone);
    connect(m_actionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FleetWindow::onActionChanged);
    connect(m_runBtn, &QPushButton::clicked, this, &FleetWindow::onRunClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &FleetWindow::onCancelClicked);
    connect(m_resultsTable, &QTableWidget::cellDoubleClicked, this, &FleetWindow::onResultRowActivated);

    onActionChanged(m_actionCombo->currentIndex());
}


void FleetWindow::appendLog(const QString& line)
{
    if (!m_log) return;
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_log->appendPlainText(QString("[%1] %2").arg(ts, line));
}

QString FleetWindow::stateToText(FleetTargetState st) const
{
    switch (st) {
        case FleetTargetState::Queued:   return "QUEUED";
        case FleetTargetState::Running:  return "RUNNING";
        case FleetTargetState::Ok:       return "OK";
        case FleetTargetState::Failed:   return "FAIL";
        case FleetTargetState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

void FleetWindow::rebuildTargetsList()
{
    if (!m_targetsList) return;

    const QString filter = m_filterEdit ? m_filterEdit->text().trimmed().toLower() : QString();

    m_targetsList->clear();

    // Order by group then name (like your main list)
    QVector<int> order;
    order.reserve(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) order.push_back(i);

    std::sort(order.begin(), order.end(), [this](int ia, int ib) {
        const auto& a = m_profiles[ia];
        const auto& b = m_profiles[ib];

        const QString ga = normalizedGroup(a.group).toLower();
        const QString gb = normalizedGroup(b.group).toLower();
        if (ga != gb) return ga < gb;

        return a.name.toLower() < b.name.toLower();
    });

    QString currentGroup;

    for (int idx : order) {
        const auto& p = m_profiles[idx];

        const QString g = normalizedGroup(p.group);
        const QString label = QString("%1  (%2@%3:%4)")
                                  .arg(p.name,
                                       p.user,
                                       p.host)
                                  .arg((p.port > 0) ? p.port : 22);

        const QString hay = (p.name + " " + p.user + " " + p.host + " " + g).toLower();
        if (!filter.isEmpty() && !hay.contains(filter))
            continue;

        if (g != currentGroup) {
            currentGroup = g;

            auto* hdr = new QListWidgetItem(currentGroup.toUpper(), m_targetsList);
            hdr->setFlags(Qt::NoItemFlags);
            hdr->setData(Qt::UserRole, -1);
            hdr->setSizeHint(QSize(hdr->sizeHint().width(), 24));
        }

        auto* it = new QListWidgetItem("    " + label, m_targetsList);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
        it->setData(Qt::UserRole, idx);
        it->setToolTip(QString("%1@%2:%3  [%4]")
                           .arg(p.user, p.host)
                           .arg((p.port > 0) ? p.port : 22)
                           .arg(normalizedGroup(p.group)));
        it->setSizeHint(QSize(it->sizeHint().width(), 24));
    }
}

void FleetWindow::onFilterChanged(const QString&)
{
    rebuildTargetsList();
}

void FleetWindow::onSelectAll()
{
    if (!m_targetsList) return;
    for (int r = 0; r < m_targetsList->count(); ++r) {
        auto* it = m_targetsList->item(r);
        if (!it || isHeaderItem(it)) continue;
        it->setCheckState(Qt::Checked);
    }
}

void FleetWindow::onSelectNone()
{
    if (!m_targetsList) return;
    for (int r = 0; r < m_targetsList->count(); ++r) {
        auto* it = m_targetsList->item(r);
        if (!it || isHeaderItem(it)) continue;
        it->setCheckState(Qt::Unchecked);
    }
}

QVector<int> FleetWindow::selectedProfileIndexes() const
{
    QVector<int> out;
    if (!m_targetsList) return out;

    for (int r = 0; r < m_targetsList->count(); ++r) {
        auto* it = m_targetsList->item(r);
        if (!it || isHeaderItem(it)) continue;
        if (it->checkState() == Qt::Checked) {
            const int idx = it->data(Qt::UserRole).toInt();
            if (idx >= 0 && idx < m_profiles.size())
                out.push_back(idx);
        }
    }
    return out;
}

void FleetWindow::clearResults()
{
    m_rowByProfile.clear();
    if (m_resultsTable) {
        m_resultsTable->setRowCount(0);
        m_resultsTable->clearContents();
    }
}

void FleetWindow::upsertResultRow(const FleetTargetResult& r)
{
    if (!m_resultsTable) return;

    int row = -1;
    if (m_rowByProfile.contains(r.profileIndex)) {
        row = m_rowByProfile.value(r.profileIndex);
    } else {
        row = m_resultsTable->rowCount();
        m_resultsTable->insertRow(row);
        m_rowByProfile.insert(r.profileIndex, row);
    }

    const QString target = QString("%1@%2:%3").arg(r.user, r.host).arg(r.port);
    const QString dur = (r.durationMs > 0) ? QString("%1 ms").arg(r.durationMs) : QString();

    auto preview = [](const QString& s) -> QString {
        QString t = s;
        t.replace("\r\n", "\n");
        t.replace("\r", "\n");
        t = t.trimmed();
        if (t.size() > 120) t = t.left(120) + "…";
        return t;
    };

    const QString outPrev = preview(r.stdoutText);
    const QString errPrev = preview(!r.error.isEmpty() ? r.error : r.stderrText);

    auto setCell = [this, row](int col, const QString& text) {
        auto* it = m_resultsTable->item(row, col);
        if (!it) {
            it = new QTableWidgetItem();
            m_resultsTable->setItem(row, col, it);
        }
        it->setText(text);
    };

    setCell(0, r.profileName);
    setCell(1, normalizedGroup(r.group));
    setCell(2, target);
    setCell(3, stateToText(r.state));
    setCell(4, dur);
    setCell(5, outPrev);
    setCell(6, errPrev);

    // stash full detail in UserRole
    auto* keyItem = m_resultsTable->item(row, 0);
    if (keyItem) {
        keyItem->setData(Qt::UserRole, r.profileIndex);
        keyItem->setData(Qt::UserRole + 1, r.stdoutText);
        keyItem->setData(Qt::UserRole + 2, r.stderrText);
        keyItem->setData(Qt::UserRole + 3, r.error);
    }
}

void FleetWindow::onActionChanged(int idx)
{
    if (!m_actionCombo || !m_actionStack) return;

    const FleetActionType t = (FleetActionType)m_actionCombo->itemData(idx).toInt();

    if (t == FleetActionType::RunCommand) {
        m_actionStack->setCurrentIndex(0);
    } else if (t == FleetActionType::CheckService) {
        m_actionStack->setCurrentIndex(1);
    } else {
        m_actionStack->setCurrentIndex(2);
    }
}

void FleetWindow::onRunClicked()
{
    if (!m_exec || m_exec->isRunning())
        return;

    const QVector<int> targets = selectedProfileIndexes();
    if (targets.isEmpty()) {
        QMessageBox::information(this, "Fleet", "Select at least one target profile.");
        return;
    }

    const int conc = m_concurrencySpin ? m_concurrencySpin->value() : 4;
    m_exec->setMaxConcurrency(conc);

    const int timeoutSec = m_timeoutSpin ? m_timeoutSpin->value() : 90;
    m_exec->setCommandTimeoutMs(timeoutSec * 1000);

    // Command audit logging mode from settings:
    // 0=None, 1=Safe (head+hash), 2=Full (store full command string)
    QSettings s;
    int cmdLogMode = s.value("audit/commandLogMode", 1).toInt();
    if (cmdLogMode < 0) cmdLogMode = 0;
    if (cmdLogMode > 2) cmdLogMode = 2;

    // Extra safety: confirm when Full mode is selected
    if (cmdLogMode == 2) {
        const auto ans = QMessageBox::warning(
            this,
            "Audit logging: FULL commands",
            "You selected FULL command logging for audit logs.\n\n"
            "This stores the entire command string into the audit log file and may include secrets "
            "(tokens, passwords, etc.).\n\n"
            "Continue?",
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel
        );
        if (ans != QMessageBox::Yes)
            return;
    }

    FleetAction action;
    action.type = (FleetActionType)m_actionCombo->currentData().toInt();

    if (action.type == FleetActionType::RunCommand) {
        action.payload = m_cmdEdit ? m_cmdEdit->text() : QString();
        action.title   = "Run command";
        if (action.payload.trimmed().isEmpty()) {
            QMessageBox::warning(this, "Fleet", "Command is empty.");
            return;
        }
    } else if (action.type == FleetActionType::CheckService) {
        action.payload = m_serviceEdit ? m_serviceEdit->text() : QString();
        action.title   = "Check service";
        if (action.payload.trimmed().isEmpty()) {
            QMessageBox::warning(this, "Fleet", "Service name is empty.");
            return;
        }
    } else if (action.type == FleetActionType::RestartService) {
        action.payload = m_serviceEdit ? m_serviceEdit->text() : QString();
        action.title   = "Restart service";
        if (action.payload.trimmed().isEmpty()) {
            QMessageBox::warning(this, "Fleet", "Service name is empty.");
            return;
        }
        if (m_confirmDanger && !m_confirmDanger->isChecked()) {
            QMessageBox::warning(this, "Fleet", "Confirm the disruptive action checkbox to proceed.");
            return;
        }
        const auto ans = QMessageBox::question(
            this,
            "Confirm restart",
            QString("Restart service '%1' on %2 target(s)?").arg(action.payload).arg(targets.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel
        );
        if (ans != QMessageBox::Yes)
            return;
    }

    clearResults();

    auto modeText = [](int m) -> QString {
        switch (m) {
            case 0: return "None";
            case 1: return "Safe";
            case 2: return "Full";
        }
        return "Safe";
    };

    appendLog(QString("Starting job on %1 target(s), concurrency=%2, timeout=%3s, auditCmd=%4")
                  .arg(targets.size())
                  .arg(conc)
                  .arg(timeoutSec)
                  .arg(modeText(cmdLogMode)));

    m_exec->start(m_profiles, targets, action);

    QJsonObject f;
    f["targets"] = (int)targets.size();
    f["concurrency"] = conc;
    f["timeout_ms"] = timeoutSec * 1000;
    f["action"] = (int)action.type;
    f["cmd_log_mode"] = cmdLogMode;

    f["payload_hash"] = QString::fromLatin1(
        QCryptographicHash::hash(action.payload.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)
    );

    AuditLogger::writeEvent("fleet.job.start", f);

    if (m_runBtn) m_runBtn->setEnabled(false);
    if (m_cancelBtn) m_cancelBtn->setEnabled(true);
}



void FleetWindow::onCancelClicked()
{
    if (!m_exec || !m_exec->isRunning()) return;
    appendLog("Cancel requested.");
    m_exec->cancel();
}

void FleetWindow::onJobStarted(const FleetJob& job)
{
    if (m_statusLabel)
        m_statusLabel->setText(QString("Running: %1").arg(job.title));

    appendLog(QString("JOB %1 started: %2").arg(job.id, job.title));
}

void FleetWindow::onJobProgress(const FleetJob& job, int done, int total)
{
    // Update rows for any results we’ve received so far
    // (job.results grows as chunks finish)
    for (const auto& r : job.results)
        upsertResultRow(r);

    if (m_statusLabel)
        m_statusLabel->setText(QString("Progress: %1/%2").arg(done).arg(total));
}

void FleetWindow::onJobFinished(const FleetJob& job)
{
    for (const auto& r : job.results)
        upsertResultRow(r);

    int ok = 0, fail = 0, cancel = 0;
    for (const auto& r : job.results) {
        if (r.state == FleetTargetState::Ok) ok++;
        else if (r.state == FleetTargetState::Canceled) cancel++;
        else if (r.state == FleetTargetState::Failed) fail++;
    }

    const QString summary =
        QString("Finished: OK=%1  FAIL=%2  CANCELED=%3  (targets=%4)")
            .arg(ok).arg(fail).arg(cancel).arg(job.results.size());

    appendLog(summary);

    if (m_statusLabel)
        m_statusLabel->setText(summary);

    if (m_runBtn) m_runBtn->setEnabled(true);
    if (m_cancelBtn) m_cancelBtn->setEnabled(false);
}

void FleetWindow::onResultRowActivated(int row, int /*col*/)
{
    if (!m_resultsTable) return;

    auto* it = m_resultsTable->item(row, 0);
    if (!it) return;

    const int profileIndex = it->data(Qt::UserRole).toInt();
    const QString out = it->data(Qt::UserRole + 1).toString();
    const QString err = it->data(Qt::UserRole + 2).toString();
    const QString hi  = it->data(Qt::UserRole + 3).toString();

    QString title = "Fleet result";
    if (profileIndex >= 0 && profileIndex < m_profiles.size())
        title = "Fleet result — " + m_profiles[profileIndex].name;

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowTitle(title);
    dlg->resize(980, 720);

    auto* v = new QVBoxLayout(dlg);

    auto* meta = new QLabel(dlg);
    if (profileIndex >= 0 && profileIndex < m_profiles.size()) {
        const auto& p = m_profiles[profileIndex];
        meta->setText(QString("<b>%1</b> — %2@%3:%4 — group: %5")
                      .arg(p.name, p.user, p.host)
                      .arg((p.port > 0) ? p.port : 22)
                      .arg(normalizedGroup(p.group)));
    }
    v->addWidget(meta);

    auto* tabs = new QTabWidget(dlg);

    auto* outView = new QTextEdit(dlg);
    outView->setReadOnly(true);
    outView->setPlainText(out);

    auto* errView = new QTextEdit(dlg);
    errView->setReadOnly(true);
    errView->setPlainText(err);

    auto* hiView = new QTextEdit(dlg);
    hiView->setReadOnly(true);
    hiView->setPlainText(hi);

    tabs->addTab(outView, "Stdout");
    tabs->addTab(errView, "Stderr");
    tabs->addTab(hiView,  "Error");

    v->addWidget(tabs, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    v->addWidget(bb);

    dlg->show();
}
