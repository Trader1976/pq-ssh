// src/Audit/AuditLogViewerDialog.cpp
#include "AuditLogViewerDialog.h"
#include "AuditLogDelegate.h"
#include "../AuditLogger.h"   // for current dir/path helpers once you add them

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateEdit>
#include <QLineEdit>
#include <QToolButton>
#include <QTableView>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QTextBrowser>

class AuditFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (filterRegularExpression().pattern().trimmed().isEmpty())
            return true;

        // Search across Event, Target, Summary, Session
        for (int c = 0; c < AuditLogModel::ColCount; ++c) {
            if (c == AuditLogModel::TimeCol) continue;
            const QModelIndex ix = sourceModel()->index(sourceRow, c, sourceParent);
            const QString s = ix.data(Qt::DisplayRole).toString();
            if (s.contains(filterRegularExpression()))
                return true;
        }

        // Also search raw JSON string (optional but powerful)
        const QModelIndex rawIx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QJsonObject raw = rawIx.data(Qt::UserRole+1).toJsonObject();
        const QString rawStr = QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Compact));
        return rawStr.contains(filterRegularExpression());
    }
};

AuditLogViewerDialog::AuditLogViewerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Audit Log Viewer");
    resize(1100, 720);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10,10,10,10);
    outer->setSpacing(8);

    // --- Top bar
    auto* top = new QHBoxLayout();
    top->setSpacing(8);

    m_date = new QDateEdit(QDate::currentDate(), this);
    m_date->setCalendarPopup(true);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText("Search event/target/session/message…");

    m_reloadBtn = new QToolButton(this);
    m_reloadBtn->setText("Reload");

    m_openDirBtn = new QToolButton(this);
    m_openDirBtn->setText("Open folder");

    top->addWidget(m_date, 0);
    top->addWidget(m_search, 1);
    top->addWidget(m_reloadBtn, 0);
    top->addWidget(m_openDirBtn, 0);
    outer->addLayout(top);

    // --- Main split: table + details
    auto* mid = new QHBoxLayout();
    mid->setSpacing(10);

    m_model = new AuditLogModel(this);
    m_proxy = new AuditFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_table = new QTableView(this);
    m_table->setModel(m_proxy);
    m_table->setItemDelegate(new AuditLogDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(false);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::TimeCol, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::SevCol,  QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::EventCol,QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::TargetCol,QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::SessionCol,QHeaderView::ResizeToContents);

    m_details = new QTextBrowser(this);
    m_details->setOpenExternalLinks(false);
    m_details->setMinimumWidth(420);

    mid->addWidget(m_table, 1);
    mid->addWidget(m_details, 0);
    outer->addLayout(mid, 1);

    connect(m_reloadBtn, &QToolButton::clicked, this, &AuditLogViewerDialog::onReload);
    connect(m_openDirBtn, &QToolButton::clicked, this, &AuditLogViewerDialog::onOpenAuditDir);
    connect(m_search, &QLineEdit::textChanged, this, &AuditLogViewerDialog::onSearchChanged);
    connect(m_date, &QDateEdit::dateChanged, this, [this](const QDate& d){ loadDate(d); });

    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex&, const QModelIndex&){ onRowChanged(); });

    loadDate(m_date->date());
}

QString AuditLogViewerDialog::auditBaseDir() const
{
    // Once you add override support in AuditLogger, use AuditLogger::auditDir().
    // For now this matches your current implementation default:
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/audit";
}

QString AuditLogViewerDialog::auditFileForDate(const QDate& d) const
{
    return QDir(auditBaseDir()).filePath(QString("audit-%1.jsonl").arg(d.toString("yyyy-MM-dd")));
}

void AuditLogViewerDialog::loadDate(const QDate& d)
{
    const QString file = auditFileForDate(d);

    QString err;
    m_model->clear();
    m_details->clear();

    if (!m_model->loadFromFile(file, &err)) {
        m_details->setHtml(
            QString("<h3>No audit file for %1</h3>"
                    "<p><code>%2</code></p>"
                    "<p>%3</p>")
                .arg(d.toString(Qt::ISODate),
                     file.toHtmlEscaped(),
                     err.toHtmlEscaped())
        );
        return;
    }

    m_table->sortByColumn(AuditLogModel::TimeCol, Qt::DescendingOrder);

    if (m_proxy->rowCount() > 0)
        m_table->selectRow(0);
}

void AuditLogViewerDialog::onReload()
{
    loadDate(m_date->date());
}

void AuditLogViewerDialog::onOpenAuditDir()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(auditBaseDir()));
}

void AuditLogViewerDialog::onSearchChanged(const QString& t)
{
    m_proxy->setFilterRegularExpression(QRegularExpression(QRegularExpression::escape(t), QRegularExpression::CaseInsensitiveOption));
    if (m_proxy->rowCount() > 0)
        m_table->selectRow(0);
}

void AuditLogViewerDialog::onRowChanged()
{
    const QModelIndex cur = m_table->currentIndex();
    if (!cur.isValid()) return;

    const QModelIndex src = m_proxy->mapToSource(cur);
    const AuditLogEntry& e = m_model->at(src.row());
    showDetails(e);
}

static QString kvRow(const QString& k, const QString& v)
{
    return QString("<tr><td style='padding:4px 10px; color:#9aa0a6; white-space:nowrap;'><b>%1</b></td>"
                   "<td style='padding:4px 10px; font-family:monospace;'>%2</td></tr>")
        .arg(k.toHtmlEscaped(), v.toHtmlEscaped());
}

void AuditLogViewerDialog::showDetails(const AuditLogEntry& e)
{
    const QJsonObject o = e.raw;

    QString html;
    html += QString("<h2 style='margin:0'>%1</h2>").arg(e.event.toHtmlEscaped());
    html += QString("<div style='color:#9aa0a6; margin:6px 0 12px 0'>%1</div>")
            .arg(e.ts.isValid() ? e.ts.toString("yyyy-MM-dd HH:mm:ss.zzz") : QString());

    html += "<table style='border-collapse:collapse; width:100%;'>";

    // Common fields first
    html += kvRow("session_id", o.value("session_id").toString());
    html += kvRow("target", e.target);
    html += kvRow("summary", e.summary);

    // “nice” fields if present
    if (o.contains("profile")) html += kvRow("profile", o.value("profile").toString());
    if (o.contains("user"))    html += kvRow("user", o.value("user").toString());
    if (o.contains("host"))    html += kvRow("host", o.value("host").toString());
    if (o.contains("port"))    html += kvRow("port", QString::number(o.value("port").toInt()));
    if (o.contains("status"))  html += kvRow("status", o.value("status").toString());
    if (o.contains("duration_ms")) html += kvRow("duration_ms", QString::number(o.value("duration_ms").toInt()));
    if (o.contains("cmd_head")) html += kvRow("cmd_head", o.value("cmd_head").toString());
    if (o.contains("cmd_hash")) html += kvRow("cmd_hash", o.value("cmd_hash").toString());

    html += "</table>";

    // Raw JSON (collapsed)
    const QString raw = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
    html += "<hr style='border:none; border-top:1px solid rgba(255,255,255,0.08); margin:14px 0'>";
    html += "<details><summary style='cursor:pointer'>Raw JSON</summary>";
    html += QString("<pre style='white-space:pre-wrap; font-family:monospace; font-size:12px;'>%1</pre>")
            .arg(raw.toHtmlEscaped());
    html += "</details>";

    m_details->setHtml(html);
}
