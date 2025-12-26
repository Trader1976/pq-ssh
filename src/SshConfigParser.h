#pragma once
//
// SshConfigParser.h
//
// PURPOSE
// -------
// Defines the minimal OpenSSH config parsing model used by pq-ssh import features.
//
// This header exposes:
// - A lightweight, ordered representation of ssh config “Host” blocks
// - A parse result container with warnings and discovered Include directives
// - Convenience getters for option lookup
//
// ARCHITECTURAL ROLE
// ------------------
// SshConfigParser is an input-only component:
//
//    ~/.ssh/config (text)
//           ↓
//    SshConfigParser::parseFile()
//           ↓
//    SshConfigParseResult (blocks + includes + warnings)
//           ↓
//    SshConfigImportPlan::buildPlan() (planning)
//           ↓
//    ImportPlanDialog + controller (review/apply into PQ-SSH profile store)
//
// Boundaries:
// - Does NOT modify ssh config
// - Does NOT apply OpenSSH matching semantics
// - Does NOT expand Include directives (v1 records only)
// - Does NOT interpret options beyond storing them as strings
//
// DATA MODEL OVERVIEW
// -------------------
// - Each "Host ..." stanza is represented as a SshConfigHostBlock.
// - Options within a block are stored as a multi-map: key -> list of values,
//   because OpenSSH allows directives to repeat (e.g., IdentityFile multiple times).
//
// GLOBAL DEFAULTS (IMPORTANT)
// --------------------------
// OpenSSH allows options before the first "Host" stanza; these act as global defaults.
// In pq-ssh v1, these are stored as a synthetic host block:
//
//   hostPatterns = ["__GLOBAL__"]
//
// This is an internal convention used by SshConfigImportPlan when
// ImportPlanOptions::applyGlobalDefaults is enabled.
//
// LIMITATIONS (v1)
// ----------------
// The parser is intentionally minimal and has known constraints:
// - Inline comment stripping is naive (everything after '#') and not quote-aware.
// - Values are not unquoted; quoted paths and escaped characters are not fully supported.
// - Include directives are recorded but not expanded/resolved.
// - No semantic merging: we preserve file structure, not OpenSSH runtime resolution.
//
// These are acceptable for v1 import and can be expanded later.

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

// Represents one parsed OpenSSH "Host ..." block.
//
// NOTE:
// A single block may contain multiple host patterns (aliases/patterns) listed on the
// "Host" line (e.g., "Host a b *.staging").
//
// In v1 import planning, pq-ssh often generates one import row per host pattern.
struct SshConfigHostBlock {
    // Patterns/aliases from "Host ..." (can be multiple).
    //
    // Special internal convention:
    // - ["__GLOBAL__"] indicates synthetic global defaults (options outside any Host stanza).
    QStringList hostPatterns;

    // Option storage as a multi-map (key -> list of values).
    //
    // - Keys are normalized to lowercase by the parser.
    // - Values are stored as raw strings (trimmed).
    // - Duplicates are preserved in-order to support directives that repeat.
    QMap<QString, QStringList> options;

    // Source metadata for traceability/debugging.
    QString sourceFile; // e.g. /home/john/.ssh/config
    int startLine = 0;  // 1-based line number where the Host block begins (or 1 for GLOBAL)
};

// Parse output container. Suitable for passing into planning/UI layers.
struct SshConfigParseResult {
    // Ordered blocks in file order, with optional synthetic "__GLOBAL__" block at front.
    QVector<SshConfigHostBlock> blocks;

    // "Include" directives discovered during parsing (not expanded in v1).
    // These are recorded for UX visibility and future feature expansion.
    QStringList includes;

    // Non-fatal parsing warnings safe to display to the user
    // (e.g., malformed Host line, unreadable file).
    QStringList warnings;
};

class SshConfigParser
{
public:
    // Parses a single OpenSSH config file at "path" and returns structured blocks.
    //
    // On failure to read:
    // - blocks may be empty
    // - warnings will include an error message
    static SshConfigParseResult parseFile(const QString& path);

    // Convenience getters for option lookup.
    // Callers should pass normalized keys (lowercase), but these functions defensively
    // lower-case the input.
    //
    // optFirst: returns first value for the key, or empty string if missing
    // optAll  : returns all values for the key, or empty list if missing
    static QString optFirst(const SshConfigHostBlock& b, const QString& keyLower);
    static QStringList optAll(const SshConfigHostBlock& b, const QString& keyLower);
};