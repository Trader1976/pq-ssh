// SshConfigParser.cpp
//
// PURPOSE
// -------
// Minimal OpenSSH config parser for pq-ssh import workflows.
//
// This parser reads an ssh config file (typically ~/.ssh/config) and produces a
// structured representation of Host blocks and their options.
//
// It is intentionally "v1 minimal":
// - Focused on the subset of SSH config needed for PQ-SSH profile import
// - Preserves enough metadata (source file + line numbers) for UX and diagnostics
// - Keeps raw option values as strings; does not attempt full semantic interpretation
//
// ARCHITECTURAL ROLE
// ------------------
// SshConfigParser is a low-level input component:
//
//    ~/.ssh/config (text)
//           ↓
//    SshConfigParser::parseFile()
//           ↓
//    SshConfigParseResult (blocks + options + includes + warnings)
//           ↓
//    SshConfigImportPlan::buildPlan()  (planning)
//           ↓
//    ImportPlanDialog (review + apply)
//
// This file must NOT:
// - write or modify SSH config
// - interpret SSH semantics deeply (match rules, conditional includes, etc.)
// - create/update PQ-SSH profiles
//
// OUTPUT MODEL
// ------------
// The output is a list of SshConfigHostBlock objects:
//
// - Each "Host ..." block becomes one SshConfigHostBlock.
// - Options before the first "Host" are treated as GLOBAL defaults and stored in a
//   synthetic block with hostPatterns = ["__GLOBAL__"].
//
// Options are stored as a multi-map (key -> list of values) because OpenSSH allows
// repeated directives (e.g., IdentityFile multiple times).
//
// PARSING LIMITATIONS (v1)
// ------------------------
// - Inline comments: anything after '#' is stripped, without quote/escape awareness.
// - Key/value splitting: uses first whitespace boundary; does not handle quoted values.
// - Include: recorded only (not expanded / not resolved / not recursively parsed).
// - No OpenSSH matching/merging semantics: we merely represent the file structure.
//
// These limitations are acceptable for a first import feature and can be expanded later.

#include "SshConfigParser.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>

static QString stripInlineComment(QString s)
{
    // OpenSSH config comments start with '#'.
    //
    // v1 policy:
    // - Remove everything after the first '#'
    // - Trim remaining whitespace
    //
    // NOTE: OpenSSH allows quoting; '#' inside quotes should not start a comment.
    // We intentionally ignore that complexity in v1.
    const int idx = s.indexOf('#');
    if (idx >= 0) s = s.left(idx);
    return s.trimmed();
}

static QString normKey(QString k)
{
    // OpenSSH keywords are case-insensitive.
    // Normalize to lowercase and trim to ensure stable lookups.
    return k.trimmed().toLower();
}

SshConfigParseResult SshConfigParser::parseFile(const QString& path)
{
    // Parses a single ssh config file into blocks and metadata.
    //
    // Returns:
    // - r.blocks: ordered list of blocks as they appear in the file
    // - r.includes: list of Include directives encountered (not expanded)
    // - r.warnings: parse-level warnings (non-fatal)
    //
    // NOTE: For includes, a future version may:
    // - resolve glob patterns
    // - recursively parse included files
    // - preserve correct ordering semantics relative to the main file
    // For v1 we merely record them.

    SshConfigParseResult r;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.warnings << QString("Cannot read %1").arg(path);
        return r;
    }

    QTextStream in(&f);
    in.setCodec("UTF-8"); // ensures deterministic behavior for non-ASCII configs

    SshConfigHostBlock current;
    bool inBlock = false;

    int lineNo = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        lineNo++;

        // Remove comments + whitespace
        line = stripInlineComment(line);
        if (line.isEmpty()) continue;

        // Split into key + rest.
        //
        // OpenSSH allows indentation; we ignore leading whitespace by trimming key anyway.
        // Format examples:
        //   Host myserver
        //   HostName example.com
        //   IdentityFile ~/.ssh/id_ed25519
        //
        // v1 behavior:
        // - Find first whitespace run
        // - key = left part (normalized)
        // - val = remaining trimmed string (can include spaces)
        const int sp = line.indexOf(QRegularExpression("\\s+"));
        QString key;
        QString val;
        if (sp < 0) {
            // Key-only directive (rare). We store empty value.
            key = normKey(line);
            val = "";
        } else {
            key = normKey(line.left(sp));
            val = line.mid(sp).trimmed();
        }

        if (key == "host") {
            // A new Host block starts.
            // Flush previous block (if any), then reset current.
            if (inBlock) r.blocks.push_back(current);

            current = SshConfigHostBlock{};
            current.sourceFile = path;
            current.startLine = lineNo;
            inBlock = true;

            // Parse patterns: "Host a b *.staging"
            // OpenSSH allows multiple patterns. We store them as-is (strings).
            current.hostPatterns = val.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (current.hostPatterns.isEmpty()) {
                // This is likely a malformed config, but we treat it as a warning only.
                r.warnings << QString("%1:%2 Host line without patterns").arg(path).arg(lineNo);
            }
            continue;
        }

        if (key == "include") {
            // v1: record only; do not expand.
            // Include semantics in OpenSSH can be complex (globs, relative paths, ordering).
            if (!val.isEmpty())
                r.includes << val;
            continue;
        }

        // Options before any Host block are “global defaults” in OpenSSH.
        // We represent them as a synthetic block:
        //   hostPatterns = ["__GLOBAL__"]
        //
        // This simplifies import planning because SshConfigImportPlan can treat it as
        // a “global defaults provider” when opt.applyGlobalDefaults is enabled.
        if (!inBlock) {
            // Create the synthetic global block once, and keep it at the front.
            if (r.blocks.isEmpty() || r.blocks.first().hostPatterns != QStringList{"__GLOBAL__"}) {
                SshConfigHostBlock g;
                g.hostPatterns = QStringList() << "__GLOBAL__";
                g.sourceFile = path;
                g.startLine = 1;
                r.blocks.push_front(g);
            }
            r.blocks.first().options[key].push_back(val);
            continue;
        }

        // Normal directive within a Host block.
        // We store a list of values to preserve repeated directives.
        current.options[key].push_back(val);
    }

    // Flush the final block if we were inside one.
    if (inBlock) r.blocks.push_back(current);
    return r;
}

QString SshConfigParser::optFirst(const SshConfigHostBlock& b, const QString& keyLower)
{
    // Convenience accessor: returns the first value for the key (if any).
    // Many OpenSSH directives can repeat; for v1 import we usually take "first wins"
    // or "first relevant", depending on higher-level logic.
    const auto it = b.options.find(keyLower.toLower());
    if (it == b.options.end() || it.value().isEmpty()) return {};
    return it.value().first();
}

QStringList SshConfigParser::optAll(const SshConfigHostBlock& b, const QString& keyLower)
{
    // Convenience accessor: returns all values for the key (may be empty).
    // Useful for directives like IdentityFile which can appear multiple times.
    const auto it = b.options.find(keyLower.toLower());
    if (it == b.options.end()) return {};
    return it.value();
}