#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

struct SshConfigHostBlock {
    QStringList hostPatterns;                 // from "Host ..." (can be multiple)
    QMap<QString, QStringList> options;       // key -> list of values (preserve duplicates)
    QString sourceFile;                       // e.g. /home/timo/.ssh/config
    int startLine = 0;                        // 1-based
};

struct SshConfigParseResult {
    QVector<SshConfigHostBlock> blocks;
    QStringList includes;                     // Include lines discovered (not expanded in v1)
    QStringList warnings;                     // parsing warnings (safe to show)
};

class SshConfigParser
{
public:
    static SshConfigParseResult parseFile(const QString& path);

    // Convenience getters (v1 focuses on these)
    static QString optFirst(const SshConfigHostBlock& b, const QString& keyLower);
    static QStringList optAll(const SshConfigHostBlock& b, const QString& keyLower);
};
