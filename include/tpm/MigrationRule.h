#ifndef TPM_MIGRATIONRULE_H
#define TPM_MIGRATIONRULE_H

#include <optional>
#include <string>
#include <vector>

// Migration-rule data model — the declaration-level semantic-transform
// schema for cross-version migration.
//
// A migration rule is *declaration-level*: it names which handler / fn /
// flow declaration changes and how. TokenStreamRewriter is only the
// landing mechanism. The structures here are the language-neutral
// data contract; parsing of `migrations/index.toml` and
// `*.migration.toml` lives in MigrationRule.cpp.

namespace tpm {

// ── rule sub-records ────────────────────────────────────────────────────

/// A record-field-level change (handler / pipeline-flow paths use it).
struct FieldChange {
    enum class Op { Add, Remove, Retype };
    Op op = Op::Add;
    std::string field;            // field name (the name half of the match key)
    std::string type;             // TypeRef — a `.topo` stdlib bridging type
    std::optional<std::string> defaultValue; // `add` may carry a default
};

/// A cross-version field/parameter name bridge — declares that an old-side
/// name carries the same identity as the named new-side name. Scope is the
/// owning rule entry's `target`.
struct NameBridge {
    std::string oldName;
    std::string newName;
};

/// A positional-parameter change on an operation-mode `fn` call site.
struct ParamChange {
    enum class Op { Insert, Remove };
    Op op = Op::Insert;
    int position = 0;             // 0-based positional index
    std::string name;
    std::string type;             // TypeRef
};

/// A pipeline / flow DAG edge change.
struct EdgeChange {
    enum class Op { Reconnect, Insert, Remove };
    Op op = Op::Reconnect;
    std::string from;             // source node (declaration simple/qualified name)
    std::string to;               // destination node
};

/// The three migration paths, keyed by the rule's `kind` field.
enum class MigrationKind {
    Handler,       // handler whose In is a record<...>
    OperationFn,   // operation-mode `fn` positional call site
    PipelineFlow,  // pipeline / flow DAG edge
};

std::optional<MigrationKind> parseMigrationKind(const std::string& text);
const char* migrationKindToString(MigrationKind kind);

/// One migration-rule entry — the minimal unit of a declaration-level
/// semantic transform.
struct MigrationRule {
    MigrationKind kind = MigrationKind::Handler;
    std::string target;                 // qualified name of the changed declaration
    std::vector<FieldChange> fieldChanges;
    std::vector<NameBridge> nameBridges;
    std::vector<ParamChange> paramChanges;
    std::vector<EdgeChange> edgeChanges;
    std::string note;                   // human-readable, copied into the report

    /// A short identifier used in the migration report's `rule` column
    /// (`kind:target`).
    std::string id() const;
};

/// One `*.migration.toml` file — the array of rule entries for one
/// version-to-version step.
struct MigrationRuleSet {
    std::vector<MigrationRule> rules;

    /// Parse a `*.migration.toml` file. On a parse / schema error returns
    /// nullopt and fills `error`.
    static std::optional<MigrationRuleSet> load(const std::string& path,
                                                std::string& error);
};

// ── version-range index ─────────────────────────────────────────────────

/// One `[[migration]]` entry of `migrations/index.toml`: a version-range
/// migration step pointing at a rule file.
///
/// Index file shape:
///   [[migration]]
///   from  = ">=0.2.0, <0.3.0"   # SemVer range
///   to    = "0.3.0"             # exact SemVer this step migrates to
///   rules = "0.2.0-to-0.3.0.migration.toml"
struct MigrationIndexEntry {
    std::string fromRange;   // SemVer range, e.g. ">=0.2.0, <0.3.0"
    std::string toVersion;   // exact SemVer the step migrates *to*
    std::string rulesFile;   // path of the rule file, relative to migrations/
};

/// `migrations/index.toml` — the version-range → rule-file index.
struct MigrationIndex {
    std::vector<MigrationIndexEntry> entries;

    /// Load `migrations/index.toml`. Returns nullopt + `error` on a parse /
    /// schema failure; an absent file is reported via `exists` (not an
    /// error — a package may simply have no `migrations/`).
    static std::optional<MigrationIndex> load(const std::string& path,
                                              bool& exists,
                                              std::string& error);

    /// Select a continuous chain of steps from `fromVersion` to
    /// `toVersion`, ordered by ascending `to`. On a coverage gap, returns
    /// nullopt and fills `error` naming the gap; the migration must not
    /// silently skip a step.
    std::optional<std::vector<MigrationIndexEntry>>
    selectPath(const std::string& fromVersion, const std::string& toVersion,
               std::string& error) const;
};

} // namespace tpm

#endif // TPM_MIGRATIONRULE_H
