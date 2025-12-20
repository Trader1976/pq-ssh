#pragma once
//
// SshConfigImportPlan.h
//
// PURPOSE
// -------
// Defines the data structures and planning API for importing OpenSSH config
// (typically ~/.ssh/config) into PQ-SSH profiles.
//
// This module produces an explicit “import plan” consisting of rows with:
//  - a proposed PQ-SSH profile (ImportedProfile)
//  - an action decision (Create / Update / Skip / Invalid)
//  - a human-readable reason (diagnostics / UX)
//  - source metadata (file, line, blockIndex) for traceability
//
// ARCHITECTURAL ROLE
// ------------------
// SshConfigImportPlan is a pure planning component:
// - Input: SshConfigParseResult (already parsed by SshConfigParser)
// - Input: existing PQ-SSH profile names (or later: existing profiles)
// - Output: QVector<ImportPlanRow> describing what should happen
//
// It intentionally does NOT:
// - parse the OpenSSH config file
// - modify ~/.ssh/config
// - write PQ-SSH profiles
// - show UI
//
// Those responsibilities belong to:
// - SshConfigParser (parsing)
// - ImportPlanDialog (review + selection)
// - ProfileStore / controller layer (apply create/update)
//
// SAFETY PRINCIPLES
// -----------------
// - Creates are selected by default (safe: no overwrites).
// - Updates are NOT selected by default (risk: overwrites existing profiles).
// - Wildcards are skipped by default (often represent templates, not concrete hosts).
//
// VERSIONING NOTE
// ---------------
// This is “v1” import: only basic fields are mapped (Host/HostName/User/Port/IdentityFile).
// Advanced directives (ProxyJump, ForwardAgent, LocalForward, etc.) are intentionally ignored
// unless later versions add explicit mapping with careful UX.

#include "SshConfigParser.h"

#include <QString>
#include <QStringList>
#include <QVector>

// Represents the subset of PQ-SSH profile fields that can be imported from OpenSSH config.
//
// NOTE:
// This is a “proposal” object. Applying it (creating/updating actual stored profiles)
// happens outside this module.
struct ImportedProfile {
    QString name;         // Profile name (derived from OpenSSH Host alias/pattern)
    QString hostName;     // HostName option; falls back to alias if empty in OpenSSH
    QString user;         // User option (may be empty)
    int     port = 22;    // Port option (default 22)
    QString identityFile; // IdentityFile option (may be empty)
};

// Action classification for each import row.
//
// - Create : profile name does not exist in PQ-SSH yet
// - Update : profile name exists and updates are allowed (opt.allowUpdates)
// - Skip   : intentionally not imported (wildcards, name collision w/ updates disabled, etc.)
// - Invalid: contains invalid data (e.g., invalid port), cannot be applied
enum class ImportAction {
    Create,
    Update,
    Skip,
    Invalid
};

// A single row in the import plan, representing one host alias/pattern from the SSH config.
struct ImportPlanRow {
    ImportAction action = ImportAction::Invalid;

    // Whether the row is selected by the user for application.
    // Only meaningful for Create/Update rows.
    bool selected = false;

    // Proposed profile data for PQ-SSH
    ImportedProfile profile;

    // Human-readable reason for action selection (diagnostics/UX).
    // Examples:
    // - "New profile will be created."
    // - "Profile with same name already exists (skipped)."
    // - "Invalid Port value: 'abc'"
    QString reason;

    // Raw host patterns in the originating OpenSSH Host block (for context/debugging).
    QStringList hostPatterns;

    // Source location metadata (helps error reporting and future diff UI).
    QString sourceFile;
    int startLine = 0;

    // Link back to the original parsed block if needed (for future advanced import).
    // This allows callers/UI to reference the raw option map without copying everything.
    int blockIndex = -1;
};

// Options controlling how the plan is generated.
struct ImportPlanOptions {
    // If true, global defaults (synthetic "__GLOBAL__" block) are applied as fallbacks.
    bool applyGlobalDefaults = true;

    // If true, host patterns containing wildcards are not imported (action = Skip).
    bool skipWildcards = true;

    // If true, profiles that already exist by name become Update candidates.
    // If false, those rows become Skip (safe default).
    bool allowUpdates = false;

    // If true, normalize/transform IdentityFile paths to reduce machine-specific paths.
    //
    // NOTE:
    // The current implementation expands "~" to an absolute home path.
    // If you intend the opposite (“compress to ~”), adjust the implementation accordingly.
    bool normalizeIdentityPath = true;

    // v1: reserved for future use. When false, future versions may include additional
    // SSH config directives into the plan/proposed profile.
    bool ignoreAdvancedOptions = true;
};

// Pure planner for converting parsed ssh config blocks into import actions + proposed profiles.
class SshConfigImportPlan
{
public:
    // Builds the plan rows from parsed OpenSSH config blocks.
    //
    // Inputs:
    // - parsed: parsed ssh config (blocks + options) from SshConfigParser
    // - existingProfileNames: used for collision detection (Create vs Update vs Skip)
    // - opt: plan-generation options (global defaults, wildcard behavior, etc.)
    //
    // Output:
    // - One ImportPlanRow per host alias/pattern, including action + reason + proposed profile.
    static QVector<ImportPlanRow> buildPlan(const SshConfigParseResult& parsed,
                                           const QStringList& existingProfileNames,
                                           const ImportPlanOptions& opt);

    // Convenience helpers for “apply” logic (typically used by the dialog/controller):
    // extract only selected Create / Update profiles from the plan rows.
    static QVector<ImportedProfile> selectedCreates(const QVector<ImportPlanRow>& rows);
    static QVector<ImportedProfile> selectedUpdates(const QVector<ImportPlanRow>& rows);

    // Counts how many rows have the given action (used for summary UI).
    static int countAction(const QVector<ImportPlanRow>& rows, ImportAction a);

private:
    // Returns true if hostPattern contains wildcard characters (simple v1 test).
    static bool isWildcard(const QString& hostPattern);

    // Normalizes "~" and "~/" in IdentityFile (current implementation expands to absolute).
    static QString normalizePathTilde(const QString& path);
};