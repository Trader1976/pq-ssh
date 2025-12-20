// SshConfigImportDialog.cpp
//
// ARCHITECTURE NOTES (SshConfigImportDialog.cpp)
//
// This dialog is a *read-only preview* of an OpenSSH client config file (~/.ssh/config).
// It exists to build user trust before import:
//   - shows parsed Host blocks and a GLOBAL defaults pseudo-block
//   - surfaces warnings (non-fatal parse issues)
//   - displays a small, stable set of “v1 columns” (HostName/User/Port/IdentityFile)
//   - counts “other options” without trying to interpret them
//
// Responsibilities:
// - UI construction (labels + table + reload/close buttons)
// - calling SshConfigParser::parseFile(...) and rendering the result
//
// Non-responsibilities:
// - Does NOT create/update PQ-SSH profiles (import plan/apply is handled elsewhere)
// - Does NOT expand Include directives (v1 parser records them only)
// - Does NOT implement SSH semantics/precedence fully (this is a lightweight preview)
//
// Data flow:
//   reload() -> SshConfigParser::parseFile(m_path) -> populate() -> table rows
//

#include "SshConfigImportDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QFileInfo>

SshConfigImportDialog::SshConfigImportDialog(const QString& configPath, QWidget* parent)
    : QDialog(parent), m_path(configPath)
{
    // Modeless window: user can keep the main app usable while inspecting config.
    setModal(false);
    setWindowTitle("CPUNK PQ-SSH — OpenSSH Config Preview");
    resize(900, 520);

    buildUi();
    reload();
}

void SshConfigImportDialog::buildUi()
{
    // Top-level layout: small margins, single column.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    // Source path title (selectable for copy/paste in bug reports).
    m_title = new QLabel(this);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_title);

    // Summary line: counts + “preview only” notice.
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);

    // Warnings block: user-safe parse warnings (not fatal).
    m_warnings = new QLabel(this);
    m_warnings->setWordWrap(true);
    outer->addWidget(m_warnings);

    // Main preview table: rows = Host blocks (including GLOBAL pseudo-block).
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({
        "Host (patterns)",
        "HostName",
        "User",
        "Port",
        "IdentityFile",
        "Other options (count)"
    });

    // UX: mostly auto-sized columns; keep last column stretch so table fills width.
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    // Read-only table with row selection (no in-place edits).
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);

    outer->addWidget(m_table, 1);

    // Button row: Reload (re-parse file) + Close.
    auto* btnRow = new QHBoxLayout();
    m_reloadBtn = new QPushButton("Reload", this);
    m_closeBtn  = new QPushButton("Close", this);

    connect(m_reloadBtn, &QPushButton::clicked, this, &SshConfigImportDialog::reload);
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::close);

    btnRow->addWidget(m_reloadBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_closeBtn);
    outer->addLayout(btnRow);
}

void SshConfigImportDialog::reload()
{
    // Parsing is cheap enough for UI-thread in v1 (single file, line-based).
    // If Include expansion is added later, consider moving parse into a worker thread.
    m_result = SshConfigParser::parseFile(m_path);
    populate();
}

static int otherOptionsCount(const SshConfigHostBlock& b)
{
    // ARCHITECTURE:
    // We deliberately avoid interpreting “advanced options” in the preview.
    // Counting them provides a hint that more exists in the block without
    // making promises about import behavior.
    //
    // Count keys excluding the v1 columns we display explicitly.
    static const QStringList known = {"hostname","user","port","identityfile"};

    int cnt = 0;
    for (auto it = b.options.begin(); it != b.options.end(); ++it) {
        if (!known.contains(it.key()))
            cnt++;
    }
    return cnt;
}

void SshConfigImportDialog::populate()
{
    // Title: absolute path for clarity.
    QFileInfo fi(m_path);
    m_title->setText(QString("Source: %1").arg(fi.absoluteFilePath()));

    // Summary: counts for quick sanity check.
    const int blocksTotal   = m_result.blocks.size();
    const int includesTotal = m_result.includes.size();
    const int warningsTotal = m_result.warnings.size();

    m_summary->setText(
        QString("Parsed %1 block(s). Includes found: %2. Warnings: %3. "
                "This is a preview only (no profiles are created yet).")
            .arg(blocksTotal)
            .arg(includesTotal)
            .arg(warningsTotal)
    );

    // Warnings: shown only when present.
    if (warningsTotal == 0) {
        m_warnings->setText("");
    } else {
        m_warnings->setText(QString("Warnings:\n• %1")
                                .arg(m_result.warnings.join("\n• ")));
    }

    // Clear and repopulate table.
    m_table->setRowCount(0);

    // Show all blocks. The parser may inject a first pseudo-block for GLOBAL defaults.
    for (const auto& b : m_result.blocks) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const QString hostLabel =
            (b.hostPatterns == (QStringList() << "__GLOBAL__"))
                ? QString("[GLOBAL DEFAULTS]")
                : b.hostPatterns.join(" ");

        const QString hostName = SshConfigParser::optFirst(b, "hostname");
        const QString user     = SshConfigParser::optFirst(b, "user");
        const QString port     = SshConfigParser::optFirst(b, "port");
        const QString idFile   = SshConfigParser::optFirst(b, "identityfile");
        const QString otherCnt = QString::number(otherOptionsCount(b));

        m_table->setItem(row, 0, new QTableWidgetItem(hostLabel));
        m_table->setItem(row, 1, new QTableWidgetItem(hostName));
        m_table->setItem(row, 2, new QTableWidgetItem(user));
        m_table->setItem(row, 3, new QTableWidgetItem(port));
        m_table->setItem(row, 4, new QTableWidgetItem(idFile));
        m_table->setItem(row, 5, new QTableWidgetItem(otherCnt));
    }

    // Final sizing pass for nice first render.
    m_table->resizeColumnsToContents();
}