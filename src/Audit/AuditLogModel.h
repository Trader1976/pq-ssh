// src/Audit/AuditLogModel.h
#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QJsonObject>
#include <QVector>

enum class AuditSeverity {
    Info,
    Ok,
    Warn,
    Error,
    Security
};

struct AuditLogEntry {
    QDateTime ts;
    QString   event;
    QString   sessionId;

    QString   target;     // derived: user@host:port or profile
    QString   summary;    // derived
    AuditSeverity sev = AuditSeverity::Info;

    QJsonObject raw;      // full JSON for details panel
};

class AuditLogModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Col { TimeCol=0, SevCol, EventCol, TargetCol, SummaryCol, SessionCol, ColCount };

    explicit AuditLogModel(QObject* parent=nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void clear();
    bool loadFromFile(const QString& filePath, QString* err=nullptr);

    const AuditLogEntry& at(int row) const { return m_items[row]; }

private:
    static bool parseLine(const QByteArray& line, AuditLogEntry* out, QString* err);
    static AuditSeverity classify(const QJsonObject& o);
    static QString deriveTarget(const QJsonObject& o);
    static QString deriveSummary(const QJsonObject& o);

private:
    QVector<AuditLogEntry> m_items;
};
