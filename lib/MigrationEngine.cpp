#include "tpm/MigrationEngine.h"

#include "tpm/DualContractVerifier.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace tpm {

bool MigrationResult::allAuto() const {
    for (const auto& e : report)
        if (e.outcome == MigrationReportEntry::Outcome::Manual) return false;
    return true;
}

size_t MigrationResult::manualCount() const {
    size_t n = 0;
    for (const auto& e : report)
        if (e.outcome == MigrationReportEntry::Outcome::Manual) ++n;
    return n;
}

namespace {

using topo::Token;
using topo::TokenKind;

// ── source text <-> (line,col) byte-offset index ────────────────────────

/// Maps a 1-based (line, column) to a byte offset into `source`.
class OffsetIndex {
public:
    explicit OffsetIndex(const std::string& source) : source_(source) {
        lineStart_.push_back(0);
        for (size_t i = 0; i < source.size(); ++i)
            if (source[i] == '\n') lineStart_.push_back(i + 1);
    }
    size_t offsetOf(int line, int column) const {
        if (line < 1) line = 1;
        size_t li = static_cast<size_t>(line - 1);
        if (li >= lineStart_.size()) return source_.size();
        size_t off = lineStart_[li] + static_cast<size_t>(column - 1);
        return std::min(off, source_.size());
    }

private:
    const std::string& source_;
    std::vector<size_t> lineStart_;
};

/// Drain a Lexer into a flat token vector (text, no comment tokens — the
/// engine anchors edits on declaration tokens, not comments).
std::vector<Token> lexAll(const std::string& source,
                          const std::string& filename) {
    topo::DiagnosticEngine diag;
    topo::Lexer lexer(source, filename, diag);
    std::vector<Token> tokens;
    for (;;) {
        Token t = lexer.nextToken();
        tokens.push_back(t);
        if (t.kind == TokenKind::Eof) break;
    }
    return tokens;
}

/// Parse + analyse a `.topo` string into a SymbolTable. `ok` reports a
/// clean frontend run (no parse / sema errors).
topo::SymbolTable analyze(const std::string& source,
                          const std::string& filename, bool& ok) {
    topo::DiagnosticEngine diag;
    topo::Lexer lexer(source, filename, diag);
    topo::Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();
    if (diag.hasErrors() || !ast) {
        ok = false;
        return topo::SymbolTable{};
    }
    topo::SemanticAnalyzer sema(diag);
    auto table = sema.analyze(static_cast<const topo::TopoFile&>(*ast));
    ok = !diag.hasErrors();
    return table;
}

// ── a pending text edit, landing a declaration-level transform ──────────

struct TextEdit {
    size_t begin = 0;   // byte offset, inclusive
    size_t end = 0;     // byte offset, exclusive
    std::string replacement;
};

/// Apply a set of non-overlapping edits to `source`, right-to-left so
/// earlier offsets stay valid.
std::string applyEdits(const std::string& source, std::vector<TextEdit> edits) {
    std::sort(edits.begin(), edits.end(),
              [](const TextEdit& a, const TextEdit& b) {
                  return a.begin > b.begin;
              });
    std::string out = source;
    for (const auto& e : edits) {
        if (e.begin > out.size() || e.end > out.size() || e.begin > e.end)
            continue;
        out.replace(e.begin, e.end - e.begin, e.replacement);
    }
    return out;
}

// ── handler-path record<...> field rewrite ──────────────────────────────

/// A located `record<...>` type occurrence in the token stream.
struct RecordSpan {
    size_t firstTok = 0;  // index of the `record` token
    size_t lastTok = 0;   // index of the closing `>` token
    std::vector<std::pair<std::string, std::string>> fields; // (name, typeText)
};

/// Render a record<...> field list back into `.topo` surface text.
std::string renderRecord(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "record<";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ", ";
        out += fields[i].first + ": " + fields[i].second;
    }
    out += ">";
    return out;
}

/// Append a type-position token to an accumulating type-text string,
/// inserting a space only where one is needed to keep the text well-formed
/// (qualified-name `::` and the angle brackets of a nested generic glue
/// tight; everything else gets a separating space).
void appendTypeToken(std::string& acc, const Token& prev, const Token& cur) {
    bool glue = acc.empty() || cur.kind == TokenKind::ColonColon ||
                prev.kind == TokenKind::ColonColon ||
                cur.kind == TokenKind::LAngle ||
                cur.kind == TokenKind::RAngle ||
                prev.kind == TokenKind::LAngle ||
                cur.kind == TokenKind::Comma;
    if (!glue) acc += " ";
    acc += cur.text;
}

/// Split a qualified name like `"orders::run"` into its segments. An
/// empty input yields an empty vector; an unqualified name yields a
/// single-segment vector.
std::vector<std::string> splitQualified(const std::string& qname) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= qname.size()) {
        size_t pos = qname.find("::", start);
        if (pos == std::string::npos) {
            if (start < qname.size()) out.push_back(qname.substr(start));
            break;
        }
        out.push_back(qname.substr(start, pos - start));
        start = pos + 2;
    }
    return out;
}

/// One reference to the rule's `target` qualified name in the token
/// stream — the anchor token's (line, column) is the per-site report
/// location for the non-handler migration paths.
struct TargetReference {
    int line = 0;
    int column = 0;
};

/// Scan the token stream for every occurrence of the qualified name
/// `target` (segments joined by `::`). The first identifier of the
/// match is the anchor location. Used by the operation-fn and
/// pipeline-flow migration paths to emit one report entry per real
/// call / edge site rather than one phantom entry per rule.
std::vector<TargetReference> findTargetReferences(
    const std::vector<Token>& toks, const std::string& target) {
    std::vector<TargetReference> refs;
    auto segments = splitQualified(target);
    if (segments.empty()) return refs;

    for (size_t i = 0; i < toks.size(); ++i) {
        // The first segment must match an Identifier token. Identifiers
        // come through the Lexer with `kind == Identifier`; some
        // soft-keywords are still Identifier-shaped here (see
        // appendTypeToken), so a text comparison is correct.
        if (toks[i].text != segments[0]) continue;
        // Bind matching segments through the `::` separators.
        size_t cursor = i;
        bool matched = true;
        for (size_t seg = 1; seg < segments.size(); ++seg) {
            if (cursor + 2 >= toks.size()) { matched = false; break; }
            if (toks[cursor + 1].kind != TokenKind::ColonColon) {
                matched = false;
                break;
            }
            if (toks[cursor + 2].text != segments[seg]) {
                matched = false;
                break;
            }
            cursor += 2;
        }
        if (!matched) continue;
        // Skip declaration sites — a `target` immediately preceded by a
        // declaration keyword (`handler`, `fn`, `flow`) is the
        // definition, not a reference. We want consumer-side sites only.
        if (i > 0) {
            TokenKind prev = toks[i - 1].kind;
            if (prev == TokenKind::KW_handler ||
                prev == TokenKind::KW_fn ||
                prev == TokenKind::KW_flow) {
                i = cursor; // advance past this binding
                continue;
            }
        }
        TargetReference ref;
        ref.line = toks[i].location.line;
        ref.column = toks[i].location.column;
        refs.push_back(ref);
        i = cursor; // advance so the rest of this qualified name is not re-scanned
    }
    return refs;
}

/// Scan a token stream for every top-level `record< ... >` occurrence.
/// Nested records / generics are captured as part of the enclosing field's
/// type text (angle depth > 1).
std::vector<RecordSpan> findRecordSpans(const std::vector<Token>& toks) {
    std::vector<RecordSpan> spans;
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind != TokenKind::KW_record) continue;
        if (toks[i + 1].kind != TokenKind::LAngle) continue;

        RecordSpan span;
        span.firstTok = i;
        int angleDepth = 1;
        std::string curName;
        std::string curType;
        bool inType = false;
        bool closed = false;
        for (size_t j = i + 2; j < toks.size(); ++j) {
            const Token& t = toks[j];
            if (t.kind == TokenKind::RAngle && angleDepth == 1) {
                if (inType && !curName.empty())
                    span.fields.emplace_back(curName, curType);
                span.lastTok = j;
                closed = true;
                break;
            }
            if (t.kind == TokenKind::Colon && angleDepth == 1 && !inType) {
                inType = true;
                continue;
            }
            if (t.kind == TokenKind::Comma && angleDepth == 1) {
                if (inType && !curName.empty())
                    span.fields.emplace_back(curName, curType);
                curName.clear();
                curType.clear();
                inType = false;
                continue;
            }
            if (t.kind == TokenKind::LAngle) ++angleDepth;
            if (t.kind == TokenKind::RAngle) --angleDepth;
            if (!inType) {
                if (curName.empty()) curName = t.text;
            } else {
                appendTypeToken(curType, toks[j - 1], t);
            }
        }
        if (closed) spans.push_back(std::move(span));
    }
    return spans;
}

/// Apply a rule's field_changes / name_bridges to a record field list.
/// Returns the new field list. `ambiguous` flags a manual-warning
/// condition (the rule mismatches the record's actual shape).
std::vector<std::pair<std::string, std::string>>
rewriteRecordFields(
    const std::vector<std::pair<std::string, std::string>>& fields,
    const MigrationRule& rule, bool& ambiguous, std::string& reason) {

    ambiguous = false;
    std::vector<std::pair<std::string, std::string>> out = fields;

    // name bridges: rename a field, type stays equal.
    for (const auto& brg : rule.nameBridges) {
        for (auto& f : out) {
            if (f.first == brg.oldName) f.first = brg.newName;
        }
    }

    for (const auto& fc : rule.fieldChanges) {
        if (fc.op == FieldChange::Op::Add) {
            bool exists = std::any_of(
                out.begin(), out.end(),
                [&](const auto& f) { return f.first == fc.field; });
            if (exists) {
                ambiguous = true;
                reason = "field '" + fc.field +
                         "' already present — `add` would duplicate it";
                return out;
            }
            out.emplace_back(fc.field, fc.type);
        } else if (fc.op == FieldChange::Op::Remove) {
            auto before = out.size();
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [&](const auto& f) {
                                         return f.first == fc.field;
                                     }),
                      out.end());
            if (out.size() == before) {
                ambiguous = true;
                reason = "field '" + fc.field +
                         "' to remove was not found in the record";
                return out;
            }
        } else { // Retype
            bool found = false;
            for (auto& f : out) {
                if (f.first == fc.field) {
                    f.second = fc.type;
                    found = true;
                }
            }
            if (!found) {
                ambiguous = true;
                reason = "field '" + fc.field +
                         "' to retype was not found in the record";
                return out;
            }
        }
    }
    return out;
}

} // namespace

// ── engine entry point ──────────────────────────────────────────────────

MigrationResult MigrationEngine::migrateSource(
    const std::string& sourcePath, const std::string& source,
    const MigrationRuleSet& rules) const {

    MigrationResult result;

    // Frontend sanity: the consumer `.topo` must parse before migration —
    // an unparseable input is a hard error, not a migration outcome.
    bool beforeOk = false;
    topo::SymbolTable beforeTable = analyze(source, sourcePath, beforeOk);
    if (!beforeOk) {
        result.ok = false;
        result.error = sourcePath +
                       ": does not parse cleanly; migration needs a "
                       "well-formed `.topo` as input";
        return result;
    }

    auto tokens = lexAll(source, sourcePath);
    OffsetIndex idx(source);

    std::vector<TextEdit> edits;
    // OLD->NEW rename map fed to the dual-contract verifier's L1 layer.
    std::vector<std::pair<std::string, std::string>> renameMap;

    for (const auto& rule : rules.rules) {
        if (rule.kind == MigrationKind::Handler) {
            // handler path: the handler's In is a record<...>.
            // Locate every record<...> in the consumer `.topo` whose field
            // set matches the rule's *old* shape (the fields the rule
            // removes / retypes / bridges must be present).
            auto spans = findRecordSpans(tokens);

            std::set<std::string> ruleFields;
            for (const auto& fc : rule.fieldChanges)
                if (fc.op != FieldChange::Op::Add)
                    ruleFields.insert(fc.field);
            for (const auto& brg : rule.nameBridges)
                ruleFields.insert(brg.oldName);

            for (const auto& span : spans) {
                std::set<std::string> present;
                for (const auto& f : span.fields) present.insert(f.first);
                // A record is a migration target when it carries every
                // pre-migration field the rule operates on. A rule with
                // only `add` changes (ruleFields empty) targets every
                // record fed to the handler — too broad to auto-pick, so
                // require at least one anchor field.
                bool isTarget = !ruleFields.empty();
                for (const auto& rf : ruleFields)
                    if (!present.count(rf)) isTarget = false;
                if (!isTarget) continue;

                bool ambiguous = false;
                std::string reason;
                auto newFields = rewriteRecordFields(span.fields, rule,
                                                     ambiguous, reason);

                MigrationReportEntry entry;
                const Token& anchor = tokens[span.firstTok];
                entry.location = sourcePath + ":" +
                                 std::to_string(anchor.location.line) + ":" +
                                 std::to_string(anchor.location.column);
                entry.declaration = rule.target;
                entry.rule = rule.id();

                if (ambiguous) {
                    entry.outcome = MigrationReportEntry::Outcome::Manual;
                    entry.reason = reason;
                    result.report.push_back(std::move(entry));
                    continue;
                }

                // A record field whose new value cannot be resolved (an
                // `add` with no default) becomes a manual warning. The
                // scope-binding source search is the operation-fn path's
                // responsibility, not the handler path's, so on this path
                // an `add` is auto only with a default.
                bool unresolved = false;
                for (const auto& fc : rule.fieldChanges) {
                    if (fc.op == FieldChange::Op::Add && !fc.defaultValue) {
                        unresolved = true;
                        entry.reason =
                            "added field '" + fc.field +
                            "' has no default value and no scope source "
                            "to fill it";
                        break;
                    }
                }
                if (unresolved) {
                    entry.outcome = MigrationReportEntry::Outcome::Manual;
                    result.report.push_back(std::move(entry));
                    continue;
                }

                TextEdit edit;
                edit.begin = idx.offsetOf(anchor.location.line,
                                          anchor.location.column);
                const Token& close = tokens[span.lastTok];
                edit.end = idx.offsetOf(close.location.line,
                                        close.location.column) + 1;
                edit.replacement = renderRecord(newFields);
                edits.push_back(std::move(edit));

                entry.outcome = MigrationReportEntry::Outcome::Auto;
                result.report.push_back(std::move(entry));
            }
        } else {
            // operation-fn and pipeline-flow paths: defined by the
            // migration model, but the MVP engine does not yet land token
            // edits for them. Every site they would touch is reported as a
            // manual warning rather than silently skipped — so the gap is
            // visible and never produces an unverified auto migration.
            //
            // We scan for every occurrence of the rule's `target`
            // qualified name in the source and emit one manual entry
            // per real site, so the report enumerates concrete
            // (line, column) anchors rather than a single phantom
            // `0:0`. When the target does not appear at all in the
            // consumer source we emit one file-level entry with an
            // explicit `?:?` location so the rule's existence is still
            // reported but no fake location is implied.
            auto refs = findTargetReferences(tokens, rule.target);
            std::string baseReason =
                std::string("the ") + migrationKindToString(rule.kind) +
                " migration path is not auto-applied in this MVP; review "
                "and migrate '" + rule.target + "' by hand";

            if (refs.empty()) {
                MigrationReportEntry entry;
                entry.location = sourcePath + ":?:?";
                entry.declaration = rule.target;
                entry.rule = rule.id();
                entry.outcome = MigrationReportEntry::Outcome::Manual;
                entry.reason = baseReason +
                    " (no consumer-side reference found in this file; "
                    "search the wider source tree by hand)";
                result.report.push_back(std::move(entry));
            } else {
                for (const auto& ref : refs) {
                    MigrationReportEntry entry;
                    entry.location = sourcePath + ":" +
                                     std::to_string(ref.line) + ":" +
                                     std::to_string(ref.column);
                    entry.declaration = rule.target;
                    entry.rule = rule.id();
                    entry.outcome = MigrationReportEntry::Outcome::Manual;
                    entry.reason = baseReason;
                    result.report.push_back(std::move(entry));
                }
            }
        }
    }

    // Land the auto edits and re-verify under the dual-contract verifier.
    if (!edits.empty()) {
        result.rewrittenSource = applyEdits(source, edits);
        result.changed = (result.rewrittenSource != source);

        // Audit fix for inconsistent report on rewrite parse failure:
        // both post-rewrite failure paths must downgrade
        // any Auto report entries to Manual. The two paths share user-
        // facing semantics — "no rewrite was committed" — so they must
        // share report shape too. The previous unparseable-rewrite branch
        // returned early without the downgrade, leaving Auto entries in
        // `result.report` that lied about having landed; any third-party
        // consumer reading the report without first checking `result.ok`
        // (the CLI does check, but library callers may not) drew the
        // wrong conclusion.
        auto holdBackAutoEntries = [&](const std::string& reason) {
            for (auto& e : result.report)
                if (e.outcome == MigrationReportEntry::Outcome::Auto) {
                    e.outcome = MigrationReportEntry::Outcome::Manual;
                    e.reason = reason;
                }
            result.rewrittenSource = source;
            result.changed = false;
        };

        bool afterOk = false;
        topo::SymbolTable afterTable =
            analyze(result.rewrittenSource, sourcePath, afterOk);
        if (!afterOk) {
            // The token edit produced an unparseable `.topo` — the rule
            // overshot what migration can express (not even the L1 layer).
            result.ok = false;
            result.error =
                sourcePath +
                ": migration produced a `.topo` that no longer parses; the "
                "rule set exceeds what declaration migration can express";
            holdBackAutoEntries(
                "held back: migration produced an unparseable .topo "
                "(rule set exceeds what declaration migration can express)");
            return result;
        }

        DualContractVerifier verifier;
        auto v = verifier.verifyL1(beforeTable, afterTable, renameMap);
        if (!v.l1Pass) {
            // L1 failure: the rewrite added / removed / reshaped a
            // declaration. This is a rule error — report and do
            // not commit the rewrite.
            result.ok = false;
            std::ostringstream os;
            os << sourcePath
               << ": dual-contract verifier L1 rejected the migration "
                  "(the rewrite changed declaration shape):";
            for (const auto& d : v.l1Differences) os << "\n  - " << d;
            result.error = os.str();
            holdBackAutoEntries(
                "held back: dual-contract verifier L1 rejected the "
                "migrated file");
            return result;
        }
        // L1 passed. L2 (stage topology / visibility) is not implemented
        // in this MVP — DualContractVerifier::l2Available() is false. An
        // L2-uncleared site must not auto-migrate. For the
        // handler path L2 is vacuous (a handler-In record reshape touches
        // no DAG edge and no visibility domain), so the handler-path auto
        // edits stand. pipeline-flow sites — the only ones L2 truly gates
        // — are already routed to manual above.
    } else {
        result.rewrittenSource = source;
        result.changed = false;
    }

    result.ok = true;
    return result;
}

} // namespace tpm
