#pragma once

#include <QString>
#include <QDateTime>

bool autoExpireMetadataFile(const QString &path, QString *errOut = nullptr, int *expiredSetOut = nullptr);
int  countExpiredKeysInMetadata(QString *errOut = nullptr);
bool parseIsoUtc(const QString &s, QDateTime &outUtc);
