// Tests for the migration engine — the three migration paths, the auto /
// manual outcomes, and the dual-contract verifier gate.

#include "tpm/MigrationEngine.h"
#include "tpm/MigrationRule.h"

#include <gtest/gtest.h>

#include <string>

using namespace tpm;

namespace {

// A consumer `.topo` whose handlers carry record<...> In/Out types — the
// canonical handler-path subject.
const char* kConsumerTopo = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> record<id: i64, amount: f64>;
    handler validate(record<id: i64, amount: f64> o) -> record<id: i64, amount: f64>;
    handler persist(record<id: i64, amount: f64> o) -> bool;

    flow pipeline {
      parse -> validate;
      validate -> persist;
      persist -> void;
    }
}
)topo";

MigrationRule makeRule(MigrationKind k, const std::string& target) {
    MigrationRule r;
    r.kind = k;
    r.target = target;
    return r;
}

} // namespace

// ── handler path — auto migration ───────────────────────────────────────

TEST(MigrationEngine, HandlerRetypeIsAutoAndRewritesRecord) {
    // Retype `amount` to f64 — the structural shape is preserved, so L1
    // passes and the outcome is auto.
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Retype;
    fc.field = "amount";
    fc.type = "f64";
    r.fieldChanges.push_back(fc);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_GE(res.report.size(), 1u);
    for (const auto& e : res.report)
        EXPECT_EQ(e.outcome, MigrationReportEntry::Outcome::Auto);
    EXPECT_TRUE(res.allAuto());
}

TEST(MigrationEngine, HandlerAddWithDefaultIsAuto) {
    // Add a `region` field with a default — the default supplies the new
    // value, so the auto-migration precondition is met and the outcome is
    // auto.
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Add;
    fc.field = "region";
    fc.type = "i64";
    fc.defaultValue = "0";
    r.fieldChanges.push_back(fc);
    // Anchor the rule to existing records via a retype so it has a target.
    FieldChange anchor;
    anchor.op = FieldChange::Op::Retype;
    anchor.field = "id";
    anchor.type = "i64";
    r.fieldChanges.push_back(anchor);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.changed);
    EXPECT_TRUE(res.allAuto());
    EXPECT_NE(res.rewrittenSource.find("region"), std::string::npos);
}

TEST(MigrationEngine, HandlerAddWithoutDefaultIsManual) {
    // Add a field with no default and no scope source — this routes
    // it to a manual warning, not a silent guess.
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Add;
    fc.field = "region";
    fc.type = "i64";
    // no defaultValue
    r.fieldChanges.push_back(fc);
    FieldChange anchor;
    anchor.op = FieldChange::Op::Retype;
    anchor.field = "id";
    anchor.type = "i64";
    r.fieldChanges.push_back(anchor);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_FALSE(res.allAuto());
    EXPECT_GT(res.manualCount(), 0u);
    // The file must not be rewritten when a site is manual-only.
    EXPECT_FALSE(res.changed);
}

TEST(MigrationEngine, HandlerNameBridgeRenamesField) {
    // The library renamed `id` to `order_id`; the name bridge carries the
    // intent and the rename is auto-applied.
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::persist");
    NameBridge brg;
    brg.oldName = "id";
    brg.newName = "order_id";
    r.nameBridges.push_back(brg);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.changed);
    EXPECT_TRUE(res.allAuto());
    EXPECT_NE(res.rewrittenSource.find("order_id"), std::string::npos);
}

TEST(MigrationEngine, HandlerRuleWithUnmatchedAnchorIsNoOp) {
    // The rule removes a field that no record carries — no record matches
    // (the field is an anchor), so the engine is a clean no-op.
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Remove;
    fc.field = "nonexistent";
    r.fieldChanges.push_back(fc);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_FALSE(res.changed);
}

// ── regression: record whose last field is a nested generic (`>>`) ──────

TEST(MigrationEngine, HandlerRecordEndingInNestedGenericIsMigrated) {
    // A spec-valid record whose final field type is a nested stdlib generic
    // closes with `>>`, which the lexer emits as a single ShiftRight token.
    // The record-span scanner must split that into two `>` so the span
    // closes; otherwise the matching record is silently skipped — no edit,
    // no report entry — while migrateSource still reports success. The
    // retype anchors on `id`, leaving the nested-generic `items` field as
    // the record's last field (the exact failing shape).
    const char* kNested = R"topo(
namespace orders {
  public:
    handler validate(record<id: i64, items: slice<i64>> o) -> bool;
}
)topo";
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Retype;
    fc.field = "id";
    fc.type = "i32";
    r.fieldChanges.push_back(fc);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kNested, rules);
    ASSERT_TRUE(res.ok) << res.error;
    // The record matched: a report entry exists and the file changed.
    ASSERT_GE(res.report.size(), 1u);
    EXPECT_TRUE(res.allAuto());
    EXPECT_TRUE(res.changed)
        << "a record ending in a nested generic (`slice<i64>>`) must be "
           "recognised and migrated, not silently skipped";
    // The retype landed and the nested generic field is preserved intact —
    // the closing `>>` must be reproduced, not mangled.
    EXPECT_NE(res.rewrittenSource.find("id: i32"), std::string::npos);
    EXPECT_NE(res.rewrittenSource.find("items: slice<i64>"),
              std::string::npos);
    EXPECT_NE(res.rewrittenSource.find("slice<i64>>"), std::string::npos)
        << "the record's closing `>>` must survive the rewrite; got:\n"
        << res.rewrittenSource;
}

// ── regression: two rules matching the same record → overlapping edits ──

TEST(MigrationEngine, OverlappingEditsFromTwoRulesAreRejected) {
    // Two handler rules both anchor on `id` and both match the SAME record
    // span, each queuing a rewrite over the identical `record<...>` byte
    // range. Applying both edits right-to-left would corrupt the rewrite
    // (the second `replace` lands inside text the first already rewrote).
    // applyEdits must detect the overlap and the engine must refuse the
    // migration as a hard error rather than emit a mangled `.topo`.
    const char* kOne = R"topo(
namespace orders {
  public:
    handler validate(record<id: i64, amount: f64> o) -> bool;
}
)topo";

    MigrationRule r1 = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange a1; a1.op = FieldChange::Op::Retype; a1.field = "id";
    a1.type = "i32";
    r1.fieldChanges.push_back(a1);

    MigrationRule r2 = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange a2; a2.op = FieldChange::Op::Retype; a2.field = "id";
    a2.type = "i16";
    r2.fieldChanges.push_back(a2);

    MigrationRuleSet rules;
    rules.rules.push_back(r1);
    rules.rules.push_back(r2);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kOne, rules);

    EXPECT_FALSE(res.ok)
        << "two rules editing the same record span must be a hard error";
    EXPECT_NE(res.error.find("overlapping"), std::string::npos)
        << "the error must name the overlapping-edit cause; got: "
        << res.error;
    // Nothing was committed: the source is unchanged and no Auto entry
    // survives claiming a landed edit.
    EXPECT_FALSE(res.changed);
    EXPECT_EQ(res.rewrittenSource, kOne);
    for (const auto& e : res.report)
        EXPECT_NE(e.outcome, MigrationReportEntry::Outcome::Auto)
            << "no report entry may claim Auto after an overlap reject";
}

TEST(MigrationEngine, SingleRuleStillRewritesAfterOverlapGuard) {
    // Guard against over-rejection: a lone rule matching one record must
    // still auto-rewrite — the overlap guard only fires on genuine
    // intersecting spans, never on a single non-overlapping edit.
    const char* kOne = R"topo(
namespace orders {
  public:
    handler validate(record<id: i64, amount: f64> o) -> bool;
}
)topo";
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc; fc.op = FieldChange::Op::Retype; fc.field = "id";
    fc.type = "i32";
    r.fieldChanges.push_back(fc);
    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kOne, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.changed);
    EXPECT_TRUE(res.allAuto());
    EXPECT_NE(res.rewrittenSource.find("id: i32"), std::string::npos);
}

// ── operation-fn / pipeline-flow paths — manual in this MVP ─────────────
//
// The MVP engine does not land token edits for these paths; per the
// fix for the phantom-location issue, the engine emits one manual
// report entry **per consumer-side reference** to the rule's target
// qualified name, and a single file-level entry with `?:?` location
// when no reference is found in this file. The earlier behaviour
// emitted a single phantom entry at `0:0` regardless, which falsely
// implied a real location.

TEST(MigrationEngine, OperationFnPathNoReferencesEmitsUnknownLocation) {
    // kConsumerTopo contains no occurrence of `orders::run`, so the
    // engine emits one file-level entry with an explicit `?:?`
    // location rather than the phantom `0:0`.
    MigrationRule r = makeRule(MigrationKind::OperationFn, "orders::run");
    ParamChange pc;
    pc.op = ParamChange::Op::Insert;
    pc.position = 1;
    pc.name = "cfg";
    pc.type = "i64";
    r.paramChanges.push_back(pc);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.report.size(), 1u);
    EXPECT_EQ(res.report[0].outcome, MigrationReportEntry::Outcome::Manual);
    EXPECT_NE(res.report[0].location.find(":?:?"), std::string::npos)
        << "no-reference report should carry an explicit unknown "
           "location, never the phantom 0:0; got: "
        << res.report[0].location;
    EXPECT_FALSE(res.changed);
}

TEST(MigrationEngine, PipelineFlowPathNoReferencesEmitsUnknownLocation) {
    // `orders::pipeline` appears in kConsumerTopo only as `flow
    // pipeline { ... }` (the declaration), which is skipped — so no
    // consumer-side reference exists and we expect the `?:?` entry.
    MigrationRule r = makeRule(MigrationKind::PipelineFlow,
                               "orders::pipeline");
    EdgeChange ec;
    ec.op = EdgeChange::Op::Insert;
    ec.from = "validate";
    ec.to = "audit";
    r.edgeChanges.push_back(ec);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.report.size(), 1u);
    EXPECT_EQ(res.report[0].outcome, MigrationReportEntry::Outcome::Manual);
    EXPECT_NE(res.report[0].location.find(":?:?"), std::string::npos)
        << "no-reference report should carry an explicit unknown "
           "location, never the phantom 0:0; got: "
        << res.report[0].location;
}

TEST(MigrationEngine, PipelineFlowPathPerEdgeSiteReport) {
    // A consumer whose flow block references the rule's target node
    // multiple times should get one manual report entry per real
    // (line, column) — never a single phantom entry at 0:0.
    const char* kFlowTopo = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> bool;
    handler validate(bool b) -> bool;
    handler persist(bool b) -> bool;

    flow pipeline {
      parse -> validate;
      validate -> persist;
      persist -> void;
    }
}
)topo";
    MigrationRule r = makeRule(MigrationKind::PipelineFlow, "validate");
    EdgeChange ec;
    ec.op = EdgeChange::Op::Insert;
    ec.from = "validate";
    ec.to = "audit";
    r.edgeChanges.push_back(ec);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("flow.topo", kFlowTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    // `validate` appears twice as a flow reference (as edge source on
    // line 8 and as edge target on line 7). The declaration site
    // (`handler validate`) is skipped via the KW_handler guard.
    ASSERT_EQ(res.report.size(), 2u);
    for (const auto& e : res.report) {
        EXPECT_EQ(e.outcome, MigrationReportEntry::Outcome::Manual);
        // Every entry carries a real (non-?) location.
        EXPECT_EQ(e.location.find(":?:?"), std::string::npos)
            << "per-site entry should carry a real location; got: "
            << e.location;
    }
}

// ── hard error: unparseable input ───────────────────────────────────────

TEST(MigrationEngine, UnparseableInputIsHardError) {
    MigrationRuleSet rules;
    rules.rules.push_back(makeRule(MigrationKind::Handler, "x"));

    MigrationEngine engine;
    auto res = engine.migrateSource("bad.topo", "this is not topo {{{", rules);
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

// ── hard error: rewrite is unparseable ──────────────────────────────────

// Regression for inconsistent report shape on a rewrite parse failure.
//
// When `migrateSource` produces a rewrite that no longer parses, both
// L1-fail and unparseable-rewrite paths share user-facing semantics —
// no rewrite was committed — and must therefore share report shape:
// every Auto entry in `result.report` is downgraded to Manual so a
// consumer reading the report alone (without first checking
// `result.ok`) is never told "this site auto-migrated successfully"
// while the source on disk is unchanged.
//
// The Retype destination type is a single closing angle bracket — the
// renderRecord helper writes it into the field's type slot, producing
// a record token sequence the lexer/parser cannot reassemble, which
// is exactly the post-rewrite parse-failure branch (~MigrationEngine.cpp
// line 410) that this test pins.
TEST(MigrationEngine, RewriteUnparseableHoldsBackAutoEntries) {
    MigrationRule r = makeRule(MigrationKind::Handler, "orders::validate");
    FieldChange fc;
    fc.op = FieldChange::Op::Retype;
    fc.field = "amount";
    fc.type = ">";          // breaks `record<…, amount: >, …>` on re-parse
    r.fieldChanges.push_back(fc);

    MigrationRuleSet rules;
    rules.rules.push_back(r);

    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);

    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
    EXPECT_NE(res.error.find("no longer parses"), std::string::npos)
        << "error should name the unparseable-rewrite branch; got: "
        << res.error;

    // No Auto entries — every one was downgraded so the report can't
    // claim a site migrated when nothing landed.
    for (const auto& e : res.report) {
        EXPECT_NE(e.outcome, MigrationReportEntry::Outcome::Auto)
            << "report kept an Auto entry after an unparseable rewrite "
               "(this is the regression the audit issue covered)";
    }
    EXPECT_FALSE(res.allAuto());
    // Source untouched — the rewrite was rolled back to the original.
    EXPECT_FALSE(res.changed);
    EXPECT_EQ(res.rewrittenSource, kConsumerTopo);
}

// ── empty rule set is a clean no-op ─────────────────────────────────────

TEST(MigrationEngine, EmptyRuleSetIsCleanNoOp) {
    MigrationRuleSet rules; // no rules
    MigrationEngine engine;
    auto res = engine.migrateSource("consumer.topo", kConsumerTopo, rules);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_FALSE(res.changed);
    EXPECT_TRUE(res.report.empty());
    EXPECT_EQ(res.rewrittenSource, kConsumerTopo);
}
