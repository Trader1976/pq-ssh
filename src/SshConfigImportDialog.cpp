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
    setModal(false);
    setWindowTitle("CPUNK PQ-SSH — OpenSSH Config Preview");
    resize(900, 520);

    buildUi();
    reload();
}

void SshConfigImportDialog::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_title);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);

    m_warnings = new QLabel(this);
    m_warnings->setWordWrap(true);
    outer->addWidget(m_warnings);

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
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_table, 1);

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
    m_result = SshConfigParser::parseFile(m_path);
    populate();
}

static int otherOptionsCount(const SshConfigHostBlock& b)
{
    // count keys excluding the v1 columns
    static const QStringList known = {"hostname","user","port","identityfile"};
    int cnt = 0;
    for (auto it = b.options.begin(); it != b.options.end(); ++it) {
        if (!known.contains(it.key())) cnt++;
    }
    return cnt;
}

void SshConfigImportDialog::populate()
{
    QFileInfo fi(m_path);
    m_title->setText(QString("Source: %1").arg(fi.absoluteFilePath()));

    const int blocksTotal = m_result.blocks.size();
    const int includesTotal = m_result.includes.size();
    const int warningsTotal = m_result.warnings.size();

    m_summary->setText(QString("Parsed %1 block(s). Includes found: %2. Warnings: %3. "
                               "This is a preview only (no profiles are created yet).")
                       .arg(blocksTotal).arg(includesTotal).arg(warningsTotal));

    if (warningsTotal == 0) {
        m_warnings->setText("");
    } else {
        m_warnings->setText(QString("Warnings:\n• %1").arg(m_result.warnings.join("\n• ")));
    }

    m_table->setRowCount(0);

    // Show all blocks except GLOBAL first row? We’ll show it too, but clearly labeled.
    for (const auto& b : m_result.blocks) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const QString hostLabel = (b.hostPatterns == (QStringList() << "__GLOBAL__"))
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

    m_table->resizeColumnsToContents();
}
