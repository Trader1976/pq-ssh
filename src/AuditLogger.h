#pragma once

#include <QString>
#include <QJsonObject>

namespace AuditLogger {
    void install(const QString& appName);

    void setSessionId(const QString& sessionId);
    QString sessionId();

    // existing
    QString auditDir();
    QString currentLogFilePath();

    void writeEvent(const QString& eventName, const QJsonObject& fields = QJsonObject());

    // NEW
    void setAuditDirOverride(const QString& absoluteDirPath); // empty => use default
    QString auditDirOverride(); // optional convenience
}