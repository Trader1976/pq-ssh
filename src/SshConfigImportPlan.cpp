#include "SshConfigImportPlan.h"
#include <QDir>

static QString firstOpt(const SshConfigHostBlock& b, const QString& key)
{
    return SshConfigParser::optFirst(b, key);
}

bool SshConfigImportPlan::isWildcard(const QString& hostPattern)
{
    return hostPattern.contains('*') || hostPattern.contains('?');
}

QString SshConfigImportPlan::normalizePathTilde(const QString& path)
{
    if (path.startsWith("~/"))
        return QDir::home().filePath(path.mid(2));
    if (path == "~")
        return QDir::homePath();
    return path;
}

QVector<ImportPlanRow> SshConfigImportPlan::buildPlan(const SshConfigParseResult& parsed,
                                                      const QStringList& existingProfileNames,
                                                      const ImportPlanOptions& opt)
{
    QVector<ImportPlanRow> out;
    if (parsed.blocks.isEmpty())
        return out;

    // Find GLOBAL block (optional)
    const SshConfigHostBlock* global = nullptr;
    int globalIndex = -1;
    for (int i = 0; i < parsed.blocks.size(); ++i) {
        const auto& b = parsed.blocks[i];
        if (b.hostPatterns.size() == 1 && b.hostPatterns.first() == "__GLOBAL__") {
            global = &b;
            globalIndex = i;
            break;
        }
    }

    auto resolve = [&](const SshConfigHostBlock& b, const QString& keyLower) -> QString {
        // per-block overrides global
        const QString v = firstOpt(b, keyLower);
        if (!v.isEmpty()) return v;
        if (opt.applyGlobalDefaults && global && (&b != global))
            return firstOpt(*global, keyLower);
        return {};
    };

    for (int bi = 0; bi < parsed.blocks.size(); ++bi) {
        const auto& b = parsed.blocks[bi];

        // Skip global block itself: it’s defaults only; show in preview dialog already.
        if (bi == globalIndex) continue;
        if (b.hostPatterns.isEmpty()) continue;

        for (const QString& hostAlias : b.hostPatterns) {

            ImportPlanRow row;
            row.hostPatterns = b.hostPatterns;
            row.sourceFile = b.sourceFile;
            row.startLine = b.startLine;
            row.blockIndex = bi;

            // Wildcards handling (v1)
            if (opt.skipWildcards && isWildcard(hostAlias)) {
                row.action = ImportAction::Skip;
                row.reason = "Wildcard host pattern (skipped).";
                row.profile.name = hostAlias;
                row.profile.hostName = "";
                row.selected = false;
                out.push_back(row);
                continue;
            }

            // Resolve fields
            row.profile.name = hostAlias;

            QString hostName = resolve(b, "hostname");
            if (hostName.isEmpty()) hostName = hostAlias; // OpenSSH fallback

            QString user = resolve(b, "user");
            QString portStr = resolve(b, "port");
            QString idFile = resolve(b, "identityfile");

            if (opt.normalizeIdentityPath && !idFile.isEmpty())
                idFile = normalizePathTilde(idFile);

            int port = 22;
            if (!portStr.isEmpty()) {
                bool ok = false;
                const int p = portStr.toInt(&ok);
                if (!ok || p <= 0 || p > 65535) {
                    row.action = ImportAction::Invalid;
                    row.reason = QString("Invalid Port value: '%1'").arg(portStr);
                    row.profile.hostName = hostName;
                    row.profile.user = user;
                    row.profile.port = 22;
                    row.profile.identityFile = idFile;
                    row.selected = false;
                    out.push_back(row);
                    continue;
                }
                port = p;
            }

            row.profile.hostName = hostName;
            row.profile.user = user;
            row.profile.port = port;
            row.profile.identityFile = idFile;

            const bool exists = existingProfileNames.contains(hostAlias);

            if (!exists) {
                row.action = ImportAction::Create;
                row.reason = "New profile will be created.";
                row.selected = true; // default ON
            } else {
                if (!opt.allowUpdates) {
                    row.action = ImportAction::Skip;
                    row.reason = "Profile with same name already exists (skipped).";
                    row.selected = false;
                } else {
                    // v1: we don’t have existing profile details here, only names.
                    // Treat as Update candidate; user can choose selection.
                    row.action = ImportAction::Update;
                    row.reason = "Existing profile name matches (update allowed).";
                    row.selected = false; // default OFF for safety
                }
            }

            out.push_back(row);
        }
    }

    return out;
}

QVector<ImportedProfile> SshConfigImportPlan::selectedCreates(const QVector<ImportPlanRow>& rows)
{
    QVector<ImportedProfile> v;
    for (const auto& r : rows) {
        if (r.action == ImportAction::Create && r.selected)
            v.push_back(r.profile);
    }
    return v;
}

QVector<ImportedProfile> SshConfigImportPlan::selectedUpdates(const QVector<ImportPlanRow>& rows)
{
    QVector<ImportedProfile> v;
    for (const auto& r : rows) {
        if (r.action == ImportAction::Update && r.selected)
            v.push_back(r.profile);
    }
    return v;
}

int SshConfigImportPlan::countAction(const QVector<ImportPlanRow>& rows, ImportAction a)
{
    int n = 0;
    for (const auto& r : rows) if (r.action == a) n++;
    return n;
}
