// src/Audit/AuditLogModel.cpp
#include "AuditLogModel.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QFileInfo>

AuditLogModel::AuditLogModel(QObject* parent) : QAbstractTableModel(parent) {}

int AuditLogModel::rowCount(const QModelIndex&) const { return m_items.size(); }
int AuditLogModel::columnCount(const QModelIndex&) const { return ColCount; }

QVariant AuditLogModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch ((Col)section) {
        case TimeCol:    return "Time";
        case SevCol:     return "Level";
        case EventCol:   return "Event";
        case TargetCol:  return "Target";
        case SummaryCol: return "Summary";
        case SessionCol: return "Session";
        default: return {};
    }
}

QVariant AuditLogModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid()) return {};
    const auto& it = m_items[idx.row()];

    if (role == Qt::DisplayRole) {
        switch ((Col)idx.column()) {
            case TimeCol:    return it.ts.isValid() ? it.ts.toString("HH:mm:ss.zzz") : QString();
            case SevCol:
                switch (it.sev) {
                    case AuditSeverity::Ok: return "OK";
                    case AuditSeverity::Warn: return "WARN";
                    case AuditSeverity::Error: return "ERROR";
                    case AuditSeverity::Security: return "SEC";
                    default: return "INFO";
                }
            case EventCol:   return it.event;
            case TargetCol:  return it.target;
            case SummaryCol: return it.summary;
            case SessionCol: return it.sessionId.left(8);
            default: return {};
        }
    }

    // Give delegate access to severity + raw object
    if (role == Qt::UserRole)  return (int)it.sev;
    if (role == Qt::UserRole+1) return it.raw;

    // Tooltips
    if (role == Qt::ToolTipRole) {
        return QString("%1\n%2")
            .arg(it.event)
            .arg(it.summary);
    }

    return {};
}

void AuditLogModel::clear()
{
    beginResetModel();
    m_items.clear();
    endResetModel();
}

bool AuditLogModel::loadFromFile(const QString& filePath, QString* err)
{
    if (err) err->clear();
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QString("Cannot open %1: %2").arg(filePath, f.errorString());
        return false;
    }

    QVector<AuditLogEntry> out;
    out.reserve(2048);

    int lineNo = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        lineNo++;
        if (line.trimmed().isEmpty()) continue;

        AuditLogEntry e;
        QString perr;
        if (!parseLine(line, &e, &perr)) {
            // Skip bad lines but keep going; viewer should not die on one corrupt line.
            // If you want: store parse errors as pseudo-events.
            continue;
        }
        out.push_back(e);
    }

    beginResetModel();
    m_items = std::move(out);
    endResetModel();
    return true;
}

bool AuditLogModel::parseLine(const QByteArray& line, AuditLogEntry* out, QString* err)
{
    if (err) err->clear();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = "Invalid JSON";
        return false;
    }

    const QJsonObject o = doc.object();
    out->raw = o;

    // ts (ISO with ms)
    const QString ts = o.value("ts").toString();
    out->ts = QDateTime::fromString(ts, Qt::ISODateWithMs);
    if (!out->ts.isValid()) out->ts = QDateTime::fromString(ts, Qt::ISODate);

    out->event     = o.value("event").toString();
    out->sessionId = o.value("session_id").toString();
    out->sev       = classify(o);
    out->target    = deriveTarget(o);
    out->summary   = deriveSummary(o);

    return true;
}

AuditSeverity AuditLogModel::classify(const QJsonObject& o)
{
    // If you add "status" field later, it becomes super reliable:
    const QString status = o.value("status").toString().toLower();
    if (status == "ok") return AuditSeverity::Ok;
    if (status == "warn") return AuditSeverity::Warn;
    if (status == "fail" || status == "error") return AuditSeverity::Error;
    if (status == "canceled" || status == "cancelled") return AuditSeverity::Warn;

    const QString ev = o.value("event").toString().toLower();

    // Security-ish events (you can extend)
    if (ev.contains("key.") || ev.contains("auth") || ev.contains("hostkey") || ev.contains("authorized_key"))
        return AuditSeverity::Security;

    if (ev.contains("fail") || ev.contains("error") || ev.contains("fatal"))
        return AuditSeverity::Error;

    if (ev.contains("warn") || ev.contains("timeout"))
        return AuditSeverity::Warn;

    if (ev.contains(".ok") || ev.endsWith("ok"))
        return AuditSeverity::Ok;

    return AuditSeverity::Info;
}

QString AuditLogModel::deriveTarget(const QJsonObject& o)
{
    const QString profile = o.value("profile").toString();
    const QString user = o.value("user").toString();
    const QString host = o.value("host").toString();
    const int port = o.value("port").toInt(0);

    if (!user.isEmpty() && !host.isEmpty()) {
        if (port > 0 && port != 22) return QString("%1@%2:%3").arg(user, host).arg(port);
        return QString("%1@%2").arg(user, host);
    }

    if (!profile.isEmpty()) return profile;

    // fallback: try fields in the object
    return QString();
}

QString AuditLogModel::deriveSummary(const QJsonObject& o)
{
    const QString ev = o.value("event").toString();
    const QString msg = o.value("message").toString();
    if (!msg.trimmed().isEmpty()) return msg.trimmed();

    // Fleet-friendly summaries
    if (ev.startsWith("fleet.", Qt::CaseInsensitive)) {
        const QString action = o.value("action").toString();
        const int targets = o.value("targets").toInt(0);
        const int conc = o.value("concurrency").toInt(0);
        const int dur = o.value("duration_ms").toInt(0);

        if (ev.contains("start", Qt::CaseInsensitive))
            return QString("Fleet started (%1 targets, concurrency %2)").arg(targets).arg(conc);

        if (ev.contains("target", Qt::CaseInsensitive) && dur > 0) {
            const QString status = o.value("status").toString();
            const QString err = o.value("error").toString();
            if (!err.isEmpty())
                return QString("Target %1 in %2 ms — %3").arg(status.isEmpty() ? "done" : status).arg(dur).arg(err);
            return QString("Target done in %1 ms").arg(dur);
        }

        if (!action.isEmpty())
            return QString("Fleet event: %1").arg(action);
    }

    // Key install summaries
    if (ev.contains("key", Qt::CaseInsensitive) && ev.contains("install", Qt::CaseInsensitive)) {
        const QString path = o.value("path").toString();
        if (!path.isEmpty()) return QString("Key install: %1").arg(path);
        return "Key install activity";
    }

    // Generic fallback: print event + one/two helpful fields if present
    const QString cmdHead = o.value("cmd_head").toString();
    if (!cmdHead.isEmpty())
        return QString("%1 (%2)").arg(ev, cmdHead);

    return ev;
}
