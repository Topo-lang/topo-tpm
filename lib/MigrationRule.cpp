#include "tpm/MigrationRule.h"

#include "tpm/SemVer.h"

#include "topo/Platform/Sanitize.h"

// toml++ in header-only, no-exception mode (consistent with Manifest.cpp).
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace tpm {

// ── enum helpers ────────────────────────────────────────────────────────

std::optional<MigrationKind> parseMigrationKind(const std::string& text) {
    if (text == "handler") return MigrationKind::Handler;
    if (text == "operation-fn") return MigrationKind::OperationFn;
    if (text == "pipeline-flow") return MigrationKind::PipelineFlow;
    return std::nullopt;
}

const char* migrationKindToString(MigrationKind kind) {
    switch (kind) {
    case MigrationKind::Handler: return "handler";
    case MigrationKind::OperationFn: return "operation-fn";
    case MigrationKind::PipelineFlow: return "pipeline-flow";
    }
    return "handler";
}

std::string MigrationRule::id() const {
    return std::string(migrationKindToString(kind)) + ":" + target;
}

namespace {

std::optional<FieldChange::Op> parseFieldOp(const std::string& s) {
    if (s == "add") return FieldChange::Op::Add;
    if (s == "remove") return FieldChange::Op::Remove;
    if (s == "retype") return FieldChange::Op::Retype;
    return std::nullopt;
}

std::optional<ParamChange::Op> parseParamOp(const std::string& s) {
    if (s == "insert") return ParamChange::Op::Insert;
    if (s == "remove") return ParamChange::Op::Remove;
    return std::nullopt;
}

std::optional<EdgeChange::Op> parseEdgeOp(const std::string& s) {
    if (s == "reconnect") return EdgeChange::Op::Reconnect;
    if (s == "insert") return EdgeChange::Op::Insert;
    if (s == "remove") return EdgeChange::Op::Remove;
    return std::nullopt;
}

std::string tstr(const toml::table& t, const char* key) {
    if (auto v = t[key].value<std::string>()) return *v;
    return "";
}

} // namespace

// ── §4.3 *.migration.toml ───────────────────────────────────────────────

std::optional<MigrationRuleSet> MigrationRuleSet::load(const std::string& path,
                                                       std::string& error) {
    toml::parse_result result = toml::parse_file(path);
    if (!result) {
        std::ostringstream os;
        os << "failed to parse " << path << ": "
           << result.error().description();
        error = os.str();
        return std::nullopt;
    }
    const toml::table& tbl = result.table();

    MigrationRuleSet set;

    const toml::array* arr = tbl["rule"].as_array();
    if (!arr) {
        error = path + ": missing required [[rule]] array";
        return std::nullopt;
    }

    int idx = 0;
    for (const auto& el : *arr) {
        ++idx;
        const toml::table* rt = el.as_table();
        if (!rt) {
            error = path + ": [[rule]] entry " + std::to_string(idx) +
                    " is not a table";
            return std::nullopt;
        }
        MigrationRule rule;

        std::string kindStr = tstr(*rt, "kind");
        auto k = parseMigrationKind(kindStr);
        if (!k) {
            error = path + ": [[rule]] entry " + std::to_string(idx) +
                    " has invalid or missing `kind` '" + kindStr +
                    "' (handler / operation-fn / pipeline-flow)";
            return std::nullopt;
        }
        rule.kind = *k;

        rule.target = tstr(*rt, "target");
        if (rule.target.empty()) {
            error = path + ": [[rule]] entry " + std::to_string(idx) +
                    " is missing the required `target` qualified name";
            return std::nullopt;
        }
        rule.note = tstr(*rt, "note");

        // field_changes
        if (const toml::array* fc = (*rt)["field_changes"].as_array()) {
            for (const auto& fe : *fc) {
                const toml::table* ft = fe.as_table();
                if (!ft) {
                    error = path + ": [[rule]] " + rule.target +
                            ": a field_changes entry is not a table";
                    return std::nullopt;
                }
                FieldChange fch;
                auto op = parseFieldOp(tstr(*ft, "op"));
                if (!op) {
                    error = path + ": [[rule]] " + rule.target +
                            ": field_changes `op` must be add/remove/retype";
                    return std::nullopt;
                }
                fch.op = *op;
                fch.field = tstr(*ft, "field");
                fch.type = tstr(*ft, "type");
                if (fch.field.empty()) {
                    error = path + ": [[rule]] " + rule.target +
                            ": a field_changes entry is missing `field`";
                    return std::nullopt;
                }
                if (auto d = (*ft)["default"].value<std::string>())
                    fch.defaultValue = *d;
                rule.fieldChanges.push_back(std::move(fch));
            }
        }

        // name_bridges
        if (const toml::array* nb = (*rt)["name_bridges"].as_array()) {
            for (const auto& ne : *nb) {
                const toml::table* nt = ne.as_table();
                if (!nt) {
                    error = path + ": [[rule]] " + rule.target +
                            ": a name_bridges entry is not a table";
                    return std::nullopt;
                }
                NameBridge brg;
                brg.oldName = tstr(*nt, "old");
                brg.newName = tstr(*nt, "new");
                if (brg.oldName.empty() || brg.newName.empty()) {
                    error = path + ": [[rule]] " + rule.target +
                            ": a name_bridges entry needs both `old` and `new`";
                    return std::nullopt;
                }
                rule.nameBridges.push_back(std::move(brg));
            }
        }

        // param_changes
        if (const toml::array* pc = (*rt)["param_changes"].as_array()) {
            for (const auto& pe : *pc) {
                const toml::table* pt = pe.as_table();
                if (!pt) {
                    error = path + ": [[rule]] " + rule.target +
                            ": a param_changes entry is not a table";
                    return std::nullopt;
                }
                ParamChange pch;
                auto op = parseParamOp(tstr(*pt, "op"));
                if (!op) {
                    error = path + ": [[rule]] " + rule.target +
                            ": param_changes `op` must be insert/remove";
                    return std::nullopt;
                }
                pch.op = *op;
                if (auto p = (*pt)["position"].value<int64_t>())
                    pch.position = static_cast<int>(*p);
                pch.name = tstr(*pt, "name");
                pch.type = tstr(*pt, "type");
                rule.paramChanges.push_back(std::move(pch));
            }
        }

        // edge_changes
        if (const toml::array* ec = (*rt)["edge_changes"].as_array()) {
            for (const auto& ee : *ec) {
                const toml::table* et = ee.as_table();
                if (!et) {
                    error = path + ": [[rule]] " + rule.target +
                            ": an edge_changes entry is not a table";
                    return std::nullopt;
                }
                EdgeChange ech;
                auto op = parseEdgeOp(tstr(*et, "op"));
                if (!op) {
                    error = path + ": [[rule]] " + rule.target +
                            ": edge_changes `op` must be reconnect/insert/remove";
                    return std::nullopt;
                }
                ech.op = *op;
                ech.from = tstr(*et, "from");
                ech.to = tstr(*et, "to");
                rule.edgeChanges.push_back(std::move(ech));
            }
        }

        set.rules.push_back(std::move(rule));
    }

    // Reject duplicate (kind, target) pairs within the same file. Two
    // rules sharing `id()` would produce two indistinguishable lines in
    // the migration report and the engine would silently apply both —
    // see tpm-extensibility-fragilities §3.
    std::set<std::string> seenIds;
    for (size_t i = 0; i < set.rules.size(); ++i) {
        const std::string ruleId = set.rules[i].id();
        if (!seenIds.insert(ruleId).second) {
            error = path + ": duplicate rule id '" + ruleId +
                    "' (rule " + std::to_string(i + 1) +
                    " shares its (kind, target) with an earlier rule in "
                    "the same file — the migration report would not be "
                    "able to distinguish them)";
            return std::nullopt;
        }
    }

    return set;
}

// ── §4.2 migrations/index.toml ──────────────────────────────────────────

std::optional<MigrationIndex> MigrationIndex::load(const std::string& path,
                                                   bool& exists,
                                                   std::string& error) {
    exists = false;
    {
        std::ifstream probe(path);
        if (!probe) {
            // Absent file — a package without a migrations/ directory.
            return MigrationIndex{};
        }
    }
    exists = true;

    toml::parse_result result = toml::parse_file(path);
    if (!result) {
        std::ostringstream os;
        os << "failed to parse " << path << ": "
           << result.error().description();
        error = os.str();
        return std::nullopt;
    }
    const toml::table& tbl = result.table();

    MigrationIndex index;
    const toml::array* arr = tbl["migration"].as_array();
    if (!arr) {
        error = path + ": missing required [[migration]] array";
        return std::nullopt;
    }
    for (const auto& el : *arr) {
        const toml::table* mt = el.as_table();
        if (!mt) {
            error = path + ": a [[migration]] entry is not a table";
            return std::nullopt;
        }
        MigrationIndexEntry e;
        e.fromRange = tstr(*mt, "from");
        e.toVersion = tstr(*mt, "to");
        e.rulesFile = tstr(*mt, "rules");
        if (e.fromRange.empty() || e.toVersion.empty() || e.rulesFile.empty()) {
            error = path + ": every [[migration]] entry needs `from`, `to` "
                           "and `rules`";
            return std::nullopt;
        }
        // Audit issue tpm-path-traversal-via-untrusted-manifest-fields:
        // ``rules`` is later joined into ``migrationsDir / rulesFile``
        // and passed to ``toml::parse_file``, so a malicious package
        // could read arbitrary files (information disclosure via the
        // parse-error message) by shipping ``rules = "../../etc/passwd"``.
        // Constrain ``rules`` to a bare filename — sibling-only lookup
        // under the package's ``migrations/`` directory is the documented
        // contract.
        if (!topo::platform::is_safe_basename(e.rulesFile)) {
            error = path + ": [[migration]] `rules` '" + e.rulesFile +
                    "' must be a bare filename (no '/', '\\', '..', "
                    "leading '-', or shell metacharacters); path-"
                    "traversal payload rejected";
            return std::nullopt;
        }
        if (!VersionReq::parse(e.fromRange)) {
            error = path + ": [[migration]] `from` '" + e.fromRange +
                    "' is not a parseable SemVer range";
            return std::nullopt;
        }
        if (!SemVer::parse(e.toVersion)) {
            error = path + ": [[migration]] `to` '" + e.toVersion +
                    "' is not valid SemVer";
            return std::nullopt;
        }
        index.entries.push_back(std::move(e));
    }
    return index;
}

std::optional<std::vector<MigrationIndexEntry>>
MigrationIndex::selectPath(const std::string& fromVersion,
                           const std::string& toVersion,
                           std::string& error) const {
    auto from = SemVer::parse(fromVersion);
    auto to = SemVer::parse(toVersion);
    if (!from) {
        error = "current version '" + fromVersion + "' is not valid SemVer";
        return std::nullopt;
    }
    if (!to) {
        error = "target version '" + toVersion + "' is not valid SemVer";
        return std::nullopt;
    }
    if (to->compare(*from) == 0) {
        // Already at target — nothing to migrate.
        return std::vector<MigrationIndexEntry>{};
    }
    if (to->compare(*from) < 0) {
        error = "target version " + toVersion + " is older than the current "
                "version " + fromVersion + "; downgrade migration is not "
                "supported";
        return std::nullopt;
    }

    // Greedily chain steps: from the current version, find a step whose
    // `from` range covers it; advance to that step's `to`; repeat until the
    // target is reached. Ordered by ascending `to` (§4.2).
    std::vector<MigrationIndexEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const MigrationIndexEntry& a, const MigrationIndexEntry& b) {
                  auto av = SemVer::parse(a.toVersion);
                  auto bv = SemVer::parse(b.toVersion);
                  return av->compare(*bv) < 0;
              });

    std::vector<MigrationIndexEntry> path;
    SemVer cursor = *from;
    while (cursor.compare(*to) < 0) {
        const MigrationIndexEntry* next = nullptr;
        for (const auto& e : sorted) {
            auto req = VersionReq::parse(e.fromRange);
            auto stepTo = SemVer::parse(e.toVersion);
            // The step must accept the cursor as its starting point and
            // move strictly forward but not past the target.
            if (req && req->matches(cursor) && stepTo &&
                stepTo->compare(cursor) > 0 && stepTo->compare(*to) <= 0) {
                next = &e;
                break;
            }
        }
        if (!next) {
            error = "no migration step covers version " + cursor.toString() +
                    " toward " + toVersion +
                    " — the package's migrations/index.toml has a coverage "
                    "gap; migration cannot silently skip a step";
            return std::nullopt;
        }
        path.push_back(*next);
        cursor = *SemVer::parse(next->toVersion);
    }
    return path;
}

} // namespace tpm
