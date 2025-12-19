#pragma once
#include "SshConfigParser.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct ImportedProfile {
    QString name;         // Host alias
    QString hostName;     // HostName (or alias fallback)
    QString user;         // User (may be empty)
    int     port = 22;    // Port (default 22)
    QString identityFile; // IdentityFile (may be empty)
};

enum class ImportAction {
    Create,
    Update,
    Skip,
    Invalid
};

struct ImportPlanRow {
    ImportAction action = ImportAction::Invalid;
    bool selected = false;

    ImportedProfile profile;

    QString reason;          // why skip/invalid/update
    QStringList hostPatterns;// raw host patterns from block
    QString sourceFile;
    int startLine = 0;

    // For future: preserve full option map for “advanced import”
    // (We keep the block index to link back to raw data if needed.)
    int blockIndex = -1;
};

struct ImportPlanOptions {
    bool applyGlobalDefaults = true;
    bool skipWildcards = true;
    bool allowUpdates = false;        // default OFF (safe)
    bool normalizeIdentityPath = true;
    bool ignoreAdvancedOptions = true; // v1: informational only
};

class SshConfigImportPlan
{
public:
    static QVector<ImportPlanRow> buildPlan(const SshConfigParseResult& parsed,
                                           const QStringList& existingProfileNames,
                                           const ImportPlanOptions& opt);

    static QVector<ImportedProfile> selectedCreates(const QVector<ImportPlanRow>& rows);
    static QVector<ImportedProfile> selectedUpdates(const QVector<ImportPlanRow>& rows);

    static int countAction(const QVector<ImportPlanRow>& rows, ImportAction a);

private:
    static bool isWildcard(const QString& hostPattern);
    static QString normalizePathTilde(const QString& path);
};
