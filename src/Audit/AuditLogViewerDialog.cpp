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
#include <QRegularExpression>

#include <QFileDialog>
#include <QSaveFile>
#include <QTextStream>
#include <QMessageBox>

class AuditFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (filterRegularExpression().pattern().trimmed().isEmpty())
            return true;

        // Search across Event, Target, Summary, Session (skip Time)
        for (int c = 0; c < AuditLogModel::ColCount; ++c) {
            if (c == AuditLogModel::TimeCol) continue;
            const QModelIndex ix = sourceModel()->index(sourceRow, c, sourceParent);
            const QString s = ix.data(Qt::DisplayRole).toString();
            if (s.contains(filterRegularExpression()))
                return true;
        }

        // Also search raw JSON string (optional but powerful)
        const QModelIndex rawIx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QJsonObject raw = rawIx.data(Qt::UserRole + 1).toJsonObject();
        const QString rawStr = QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Compact));
        return rawStr.contains(filterRegularExpression());
    }
};

AuditLogViewerDialog::AuditLogViewerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Audit Log Viewer"));
    resize(1100, 720);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    // --- Top bar
    auto* top = new QHBoxLayout();
    top->setSpacing(8);

    m_date = new QDateEdit(QDate::currentDate(), this);
    m_date->setCalendarPopup(true);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search event/target/session/message…"));

    m_reloadBtn = new QToolButton(this);
    m_reloadBtn->setText(tr("Reload"));

    m_openDirBtn = new QToolButton(this);
    m_openDirBtn->setText(tr("Open folder"));

    m_exportBtn = new QToolButton(this);
    m_exportBtn->setText(tr("Export HTML…"));
    m_exportBtn->setToolTip(tr("Export the currently filtered audit events to a standalone HTML report"));

    top->addWidget(m_date, 0);
    top->addWidget(m_search, 1);
    top->addWidget(m_reloadBtn, 0);
    top->addWidget(m_openDirBtn, 0);
    top->addWidget(m_exportBtn, 0);
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
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::SevCol, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::EventCol, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::TargetCol, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AuditLogModel::SessionCol, QHeaderView::ResizeToContents);

    m_details = new QTextBrowser(this);
    m_details->setOpenExternalLinks(false);
    m_details->setMinimumWidth(420);

    mid->addWidget(m_table, 1);
    mid->addWidget(m_details, 0);
    outer->addLayout(mid, 1);

    connect(m_reloadBtn, &QToolButton::clicked, this, &AuditLogViewerDialog::onReload);
    connect(m_openDirBtn, &QToolButton::clicked, this, &AuditLogViewerDialog::onOpenAuditDir);
    connect(m_exportBtn, &QToolButton::clicked, this, &AuditLogViewerDialog::onExportHtml);
    connect(m_search, &QLineEdit::textChanged, this, &AuditLogViewerDialog::onSearchChanged);
    connect(m_date, &QDateEdit::dateChanged, this, [this](const QDate& d) { loadDate(d); });

    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex&, const QModelIndex&) { onRowChanged(); });

    loadDate(m_date->date());
}

QString AuditLogViewerDialog::auditBaseDir() const
{
    // Prefer AuditLogger::auditDir() once override is wired up.
    // For now, match current default:
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
            tr("<h3>No audit file for %1</h3>"
               "<p><code>%2</code></p>"
               "<p>%3</p>")
                .arg(d.toString(Qt::ISODate),
                     file.toHtmlEscaped(),
                     err.toHtmlEscaped()));
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
    m_proxy->setFilterRegularExpression(
        QRegularExpression(QRegularExpression::escape(t), QRegularExpression::CaseInsensitiveOption));

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
    html += kvRow(QStringLiteral("session_id"), o.value("session_id").toString());
    html += kvRow(tr("target"), e.target);
    html += kvRow(tr("summary"), e.summary);

    // “nice” fields if present
    if (o.contains("profile")) html += kvRow(QStringLiteral("profile"), o.value("profile").toString());
    if (o.contains("user"))    html += kvRow(QStringLiteral("user"), o.value("user").toString());
    if (o.contains("host"))    html += kvRow(QStringLiteral("host"), o.value("host").toString());
    if (o.contains("port"))    html += kvRow(QStringLiteral("port"), QString::number(o.value("port").toInt()));
    if (o.contains("status"))  html += kvRow(QStringLiteral("status"), o.value("status").toString());
    if (o.contains("duration_ms")) html += kvRow(QStringLiteral("duration_ms"), QString::number(o.value("duration_ms").toInt()));
    if (o.contains("cmd_head")) html += kvRow(QStringLiteral("cmd_head"), o.value("cmd_head").toString());
    if (o.contains("cmd_hash")) html += kvRow(QStringLiteral("cmd_hash"), o.value("cmd_hash").toString());

    html += "</table>";

    // Raw JSON (collapsed)
    const QString raw = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
    html += "<hr style='border:none; border-top:1px solid rgba(255,255,255,0.08); margin:14px 0'>";
    html += tr("<details><summary style='cursor:pointer'>Raw JSON</summary>");
    html += QString("<pre style='white-space:pre-wrap; font-family:monospace; font-size:12px;'>%1</pre>")
                .arg(raw.toHtmlEscaped());
    html += "</details>";

    m_details->setHtml(html);
}

// ------------------------
// HTML export
// ------------------------

static QString escHtml(const QString& s)
{
    return QString(s).toHtmlEscaped();
}

static QString sevClassToBadge(const QString& sev)
{
    const QString s = sev.trimmed().toLower();
    if (s.contains("fatal") || s.contains("error") || s.contains("denied") || s.contains("fail")) return "error";
    if (s.contains("warn")) return "warn";
    if (s.contains("ok") || s.contains("success")) return "ok";
    return "info";
}

void AuditLogViewerDialog::onExportHtml()
{
    const QDate d = m_date ? m_date->date() : QDate::currentDate();
    const QString suggested = tr("pq-ssh-audit-%1-%2.html")
                                  .arg(d.toString("yyyy-MM-dd"),
                                       QDateTime::currentDateTime().toString("HHmmss"));

    const QString outPath = QFileDialog::getSaveFileName(
        this,
        tr("Export audit log (HTML)"),
        QDir::home().filePath(suggested),
        tr("HTML files (*.html);;All files (*)"));

    if (outPath.isEmpty())
        return;

    const int rows = (m_proxy ? m_proxy->rowCount() : 0);
    const QString search = (m_search ? m_search->text().trimmed() : QString());
    const QString filePath = auditFileForDate(d);

    QString html;
    html.reserve(256 * 1024);

    html += "<!doctype html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + escHtml(tr("PQ-SSH Audit Report")) + "</title>";
    html += R"(
<style>
:root { color-scheme: dark; }
body { margin:0; font-family: system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, Cantarell, Arial, sans-serif;
       background:#0b0b0c; color:#e7e7ea; }
.header { padding:18px 22px; border-bottom:1px solid rgba(255,255,255,.08); }
.h1 { font-size:18px; font-weight:800; margin:0 0 6px 0; }
.meta { font-size:13px; opacity:.75; margin-top:2px; }
.wrap { padding:18px 22px; }
.table { width:100%; border-collapse: collapse; }
.table th { text-align:left; font-size:12px; opacity:.75; font-weight:700; padding:10px 10px;
            border-bottom:1px solid rgba(255,255,255,.08); }
.table td { padding:10px 10px; border-bottom:1px solid rgba(255,255,255,.06); vertical-align: top; }
.badge { display:inline-block; padding:2px 8px; border-radius:999px; font-size:12px; font-weight:800; letter-spacing:.02em; }
.badge.ok { background: rgba(0,255,153,.14); border:1px solid rgba(0,255,153,.35); color:#7cffc8; }
.badge.info { background: rgba(120,160,255,.12); border:1px solid rgba(120,160,255,.30); color:#b8ccff; }
.badge.warn { background: rgba(255,196,0,.12); border:1px solid rgba(255,196,0,.35); color:#ffe08a; }
.badge.error { background: rgba(255,82,82,.12); border:1px solid rgba(255,82,82,.35); color:#ff9c9c; }
.small { font-size: 12px; opacity: .85; }
.code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace;
        font-size: 12px; white-space: pre-wrap; word-break: break-word;
        background: rgba(255,255,255,.05); border: 1px solid rgba(255,255,255,.08);
        padding: 8px 10px; border-radius: 10px; margin-top: 8px; }
details summary { cursor:pointer; opacity:.9; }
.footer { padding: 14px 22px; opacity:.6; font-size: 12px; border-top: 1px solid rgba(255,255,255,.08); }
</style>
)";
    html += "</head><body>";

    html += "<div class='header'>";
    html += "<div class='h1'>" + escHtml(tr("PQ-SSH Audit Report")) + "</div>";
    html += "<div class='meta'>" + escHtml(tr("Date: %1").arg(d.toString(Qt::ISODate))) + "</div>";
    html += "<div class='meta'>" + escHtml(tr("Source file:")) + " <span class='small'>" + escHtml(filePath) + "</span></div>";
    html += "<div class='meta'>" + escHtml(tr("Rows exported (filtered): %1").arg(QString::number(rows))) + "</div>";
    if (!search.isEmpty())
        html += "<div class='meta'>" + escHtml(tr("Search filter:")) + " <span class='small'>" + escHtml(search) + "</span></div>";
    html += "</div>";

    html += "<div class='wrap'>";
    html += "<table class='table'>";
    html += "<thead><tr><th>" + escHtml(tr("Time")) + "</th><th>" + escHtml(tr("Severity")) + "</th><th>"
         + escHtml(tr("Event")) + "</th><th>" + escHtml(tr("Target")) + "</th><th>"
         + escHtml(tr("Session")) + "</th><th>" + escHtml(tr("Details")) + "</th></tr></thead><tbody>";

    for (int r = 0; r < rows; ++r) {
        const QModelIndex p0 = m_proxy->index(r, 0);

        // Pull display columns via proxy (what user sees)
        const QString time    = m_proxy->index(r, AuditLogModel::TimeCol).data(Qt::DisplayRole).toString();
        const QString sev     = m_proxy->index(r, AuditLogModel::SevCol).data(Qt::DisplayRole).toString();
        const QString event   = m_proxy->index(r, AuditLogModel::EventCol).data(Qt::DisplayRole).toString();
        const QString target  = m_proxy->index(r, AuditLogModel::TargetCol).data(Qt::DisplayRole).toString();
        const QString session = m_proxy->index(r, AuditLogModel::SessionCol).data(Qt::DisplayRole).toString();

#if 1
        const QString summary = m_proxy->index(r, AuditLogModel::SummaryCol).data(Qt::DisplayRole).toString();
#else
        const QString summary;
#endif

        // Raw JSON from *source* model (stored on col0 user role)
        const QModelIndex src0 = m_proxy->mapToSource(p0);
        const QJsonObject raw = src0.data(Qt::UserRole + 1).toJsonObject();
        const QString rawCompact = QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Compact));

        html += "<tr>";
        html += "<td class='small'>" + escHtml(time) + "</td>";
        html += "<td><span class='badge " + escHtml(sevClassToBadge(sev)) + "'>"
                + escHtml(sev.isEmpty() ? tr("INFO") : sev) + "</span></td>";
        html += "<td>" + escHtml(event) + "</td>";
        html += "<td class='small'>" + escHtml(target) + "</td>";
        html += "<td class='small'>" + escHtml(session.isEmpty() ? tr("-") : session) + "</td>";

        html += "<td>";
        if (!summary.isEmpty())
            html += "<div class='small'>" + escHtml(summary) + "</div>";

        html += "<details><summary>" + escHtml(tr("Raw JSON")) + "</summary>";
        html += "<div class='code'>" + escHtml(rawCompact) + "</div>";
        html += "</details>";
        html += "</td>";

        html += "</tr>";
    }

    html += "</tbody></table></div>";
    html += "<div class='footer'>" + escHtml(tr("CPUNK PQ-SSH — audit export")) + "</div>";
    html += "</body></html>";

    QSaveFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Cannot write file:\n%1").arg(f.errorString()));
        return;
    }

    QTextStream out(&f);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    out.setEncoding(QStringConverter::Utf8);
#else
    out.setCodec("UTF-8");
#endif
    out << html;

    if (!f.commit()) {
        QMessageBox::warning(this, tr("Export failed"), tr("Failed to finalize file:\n%1").arg(f.errorString()));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(outPath));
}
