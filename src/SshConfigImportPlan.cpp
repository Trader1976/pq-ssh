// SshConfigImportPlan.cpp
//
// PURPOSE
// -------
// Generates an “import plan” that maps parsed OpenSSH config host blocks into
// PQ-SSH profiles, including an action decision for each host alias:
//
//   - Create  : profile name does not exist yet in PQ-SSH
//   - Update  : profile name exists and updates are allowed
//   - Skip    : not importable (wildcard skipped, or name collision but updates disabled)
//   - Invalid : import candidate exists but contains invalid data (e.g., bad Port)
//
// This file contains the *pure planning logic*:
// - It does not render UI.
// - It does not write PQ-SSH profiles.
// - It does not modify ~/.ssh/config.
//
// ARCHITECTURAL ROLE
// ------------------
// This module sits between:
//   SshConfigParser          -> produces SshConfigParseResult (blocks, options, locations)
//   ProfileStore (or caller) -> knows existing PQ-SSH profiles and persists changes
//   ImportPlanDialog         -> displays plan + selection and triggers apply
//
// It is intentionally deterministic and side-effect free so it can be:
// - unit-tested
// - used by both GUI and future CLI workflows
//
// DESIGN CONSTRAINTS (v1)
// -----------------------
// - Collision detection is name-based only (existingProfileNames).
//   We do not load/compare existing profile content here.
// - Wildcard patterns are handled simply (skip by default if requested).
// - Global defaults are supported through a synthetic "__GLOBAL__" block.
//
// FUTURE WORK IDEAS
// -----------------
// - Improve collision handling by passing existing profiles (not just names) so “Update”
//   can become “No-op” when identical or show a diff summary.
// - Expand wildcard handling: allow importing wildcards as a “template group”
//   instead of skipping.
// - Normalize IdentityFile more robustly (quotes, multiple paths, relative paths, env vars).
// - Handle additional SSH config directives relevant to PQ-SSH profiles.

#include "SshConfigImportPlan.h"

#include <QDir>

static QString firstOpt(const SshConfigHostBlock& b, const QString& key)
{
    // Helper to retrieve the first option value for a given key from a host block.
    // Delegates parsing rules (case, duplicates) to SshConfigParser.
    return SshConfigParser::optFirst(b, key);
}

bool SshConfigImportPlan::isWildcard(const QString& hostPattern)
{
    // OpenSSH host patterns can include wildcards. We treat any '*' or '?' as wildcard.
    // NOTE: OpenSSH supports more complex matching semantics (negation, lists, etc.).
    // For v1, this simple check is enough to decide “likely wildcard”.
    return hostPattern.contains('*') || hostPattern.contains('?');
}

QString SshConfigImportPlan::normalizePathTilde(const QString& path)
{
    // Normalizes "~" and "~/" to an absolute path under the current user's home directory.
    //
    // IMPORTANT: Despite the checkbox label “Normalize ... (~)”, this function currently
    // *expands* tilde to an absolute path (QDir::home()).
    //
    // If your intended behavior is the opposite (convert /home/user/... back to ~),
    // rename this function or adjust its logic to avoid confusion.
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
    // Builds a plan row for every host alias (pattern) in every non-global block.
    //
    // Input:
    // - parsed: produced by SshConfigParser; includes blocks with hostPatterns and opt maps
    // - existingProfileNames: list of PQ-SSH profile names already present (for collision decisions)
    // - opt: plan-generation options from UI
    //
    // Output:
    // - QVector<ImportPlanRow> where each row includes:
    //   - action + reason
    //   - proposed ImportedProfile
    //   - source location metadata (file, line, block index)
    //   - selection default

    QVector<ImportPlanRow> out;
    if (parsed.blocks.isEmpty())
        return out;

    // =========================
    // Locate GLOBAL defaults block (optional)
    // =========================
    // SshConfigParser emits a synthetic "__GLOBAL__" host block for defaults if present.
    // We treat it as “defaults only”; it is never imported as a profile directly.
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

    // resolve(key): per-block overrides global; global applied only when enabled.
    auto resolve = [&](const SshConfigHostBlock& b, const QString& keyLower) -> QString {
        // Per-block value wins
        const QString v = firstOpt(b, keyLower);
        if (!v.isEmpty()) return v;

        // Fallback to global defaults (if enabled and global exists)
        if (opt.applyGlobalDefaults && global && (&b != global))
            return firstOpt(*global, keyLower);

        return {};
    };

    // =========================
    // Walk blocks and build rows
    // =========================
    for (int bi = 0; bi < parsed.blocks.size(); ++bi) {
        const auto& b = parsed.blocks[bi];

        // Skip global defaults block itself: not importable as a concrete profile.
        if (bi == globalIndex) continue;
        if (b.hostPatterns.isEmpty()) continue;

        // OpenSSH allows multiple patterns per "Host" line.
        // For v1, we generate one import row per host alias/pattern.
        for (const QString& hostAlias : b.hostPatterns) {

            ImportPlanRow row;

            // Source metadata for traceability/debugging
            row.hostPatterns = b.hostPatterns;
            row.sourceFile = b.sourceFile;
            row.startLine = b.startLine;
            row.blockIndex = bi;

            // =========================
            // Wildcard handling (v1)
            // =========================
            // Wildcards often represent templates and may not map to a single host.
            // We skip them by default when requested.
            if (opt.skipWildcards && isWildcard(hostAlias)) {
                row.action = ImportAction::Skip;
                row.reason = "Wildcard host pattern (skipped).";
                row.profile.name = hostAlias;
                row.profile.hostName = "";
                row.selected = false;
                out.push_back(row);
                continue;
            }

            // =========================
            // Resolve core fields
            // =========================
            row.profile.name = hostAlias;

            // HostName: if absent, OpenSSH falls back to the host alias itself.
            QString hostName = resolve(b, "hostname");
            if (hostName.isEmpty()) hostName = hostAlias;

            QString user = resolve(b, "user");
            QString portStr = resolve(b, "port");
            QString idFile = resolve(b, "identityfile");

            // IdentityFile normalization (currently expands "~" to absolute)
            if (opt.normalizeIdentityPath && !idFile.isEmpty())
                idFile = normalizePathTilde(idFile);

            // Port parsing/validation
            int port = 22;
            if (!portStr.isEmpty()) {
                bool ok = false;
                const int p = portStr.toInt(&ok);
                if (!ok || p <= 0 || p > 65535) {
                    // Invalid port => row is marked invalid but still shows resolved data
                    // so the user can understand what went wrong.
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

            // Fill profile proposal
            row.profile.hostName = hostName;
            row.profile.user = user;
            row.profile.port = port;
            row.profile.identityFile = idFile;

            // =========================
            // Collision decision (Create vs Update vs Skip)
            // =========================
            // In v1 we decide based on existence of a profile with the same name.
            // Later versions can compare actual profile content.
            const bool exists = existingProfileNames.contains(hostAlias);

            if (!exists) {
                row.action = ImportAction::Create;
                row.reason = "New profile will be created.";
                row.selected = true; // default ON (safe: doesn't overwrite anything)
            } else {
                if (!opt.allowUpdates) {
                    row.action = ImportAction::Skip;
                    row.reason = "Profile with same name already exists (skipped).";
                    row.selected = false;
                } else {
                    // v1: no diffing. It becomes an update candidate.
                    // Default OFF to avoid accidental overwrites.
                    row.action = ImportAction::Update;
                    row.reason = "Existing profile name matches (update allowed).";
                    row.selected = false;
                }
            }

            out.push_back(row);
        }
    }

    return out;
}

QVector<ImportedProfile> SshConfigImportPlan::selectedCreates(const QVector<ImportPlanRow>& rows)
{
    // Extracts selected Create actions for applying.
    QVector<ImportedProfile> v;
    for (const auto& r : rows) {
        if (r.action == ImportAction::Create && r.selected)
            v.push_back(r.profile);
    }
    return v;
}

QVector<ImportedProfile> SshConfigImportPlan::selectedUpdates(const QVector<ImportPlanRow>& rows)
{
    // Extracts selected Update actions for applying.
    QVector<ImportedProfile> v;
    for (const auto& r : rows) {
        if (r.action == ImportAction::Update && r.selected)
            v.push_back(r.profile);
    }
    return v;
}

int SshConfigImportPlan::countAction(const QVector<ImportPlanRow>& rows, ImportAction a)
{
    // Utility used by the UI to build summary counts.
    int n = 0;
    for (const auto& r : rows)
        if (r.action == a) n++;
    return n;
}
