// Tests for the dual-contract verifier — L1 structural verification under
// match-key / name-bridge renaming.

#include "tpm/DualContractVerifier.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Sema/SemanticAnalyzer.h"

#include <gtest/gtest.h>

#include <string>

using namespace tpm;

namespace {

/// Parse + analyse a `.topo` string into a SymbolTable.
topo::SymbolTable build(const std::string& source) {
    topo::DiagnosticEngine diag;
    topo::Lexer lexer(source, "test.topo", diag);
    topo::Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();
    EXPECT_FALSE(diag.hasErrors()) << "fixture `.topo` should parse cleanly";
    topo::SemanticAnalyzer sema(diag);
    return sema.analyze(static_cast<const topo::TopoFile&>(*ast));
}

const char* kBefore = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> record<id: i64, amount: f64>;
    handler persist(record<id: i64, amount: f64> o) -> bool;
}
)topo";

} // namespace

// ── L1: identical tables are equivalent ─────────────────────────────────

TEST(DualContractVerifier, IdenticalTablesPassL1) {
    auto a = build(kBefore);
    auto b = build(kBefore);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_TRUE(r.l1Pass);
    EXPECT_TRUE(r.l1Differences.empty());
}

// ── L1: a same-shape `.topo` passes ─────────────────────────────────────

TEST(DualContractVerifier, SameShapePassesL1) {
    // The handler still has one parameter and a bool return — the four
    // domains L1 compares are unchanged.
    const char* afterSameShape = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> record<id: i64, amount: f64>;
    handler persist(record<id: i64, amount: f64> o) -> bool;
}
)topo";
    auto a = build(kBefore);
    auto b = build(afterSameShape);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_TRUE(r.l1Pass) << (r.l1Differences.empty() ? "" : r.l1Differences[0]);
}

// ── L1: a reshape of a record nested inside a container is normalized ───

TEST(DualContractVerifier, NestedRecordReshapeIsNormalizedAwayInL1) {
    // A handler whose return/parameter type wraps a record in a stdlib
    // container (`optional<record<...>>`) is the common nullable-result
    // shape. Reshaping that nested record's fields is exactly the migration
    // delta L1 must NOT re-litigate — structuralSignature has to normalize a
    // record at any nesting depth, not only at the top level. Before the fix
    // the nested record was expanded verbatim, so this passed only for
    // top-level records and falsely rejected the nested case.
    const char* kNestedBefore = R"topo(
namespace orders {
  public:
    optional<record<id: i64, amount: f64>> lookup(string key);
    bool take(slice<record<id: i64>> rows);
}
)topo";
    const char* kNestedAfter = R"topo(
namespace orders {
  public:
    optional<record<id: i32, amount: f64, tag: string>> lookup(string key);
    bool take(slice<record<id: i32, extra: bool>> rows);
}
)topo";
    auto a = build(kNestedBefore);
    auto b = build(kNestedAfter);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_TRUE(r.l1Pass)
        << "a reshape of a record nested inside optional<...> / slice<...> "
           "must be normalized away in L1, not flagged as a shape change; "
           "first diff: "
        << (r.l1Differences.empty() ? "(none)" : r.l1Differences[0]);
}

TEST(DualContractVerifier, NestedContainerKindChangeStillFailsL1) {
    // The normalization must stay precise: changing the CONTAINER around the
    // nested record (optional -> slice) is a genuine shape change and must
    // still be caught, even though the inner record normalizes to `<...>`.
    const char* kBeforeOpt = R"topo(
namespace orders {
  public:
    optional<record<id: i64>> lookup(string key);
}
)topo";
    const char* kAfterSlice = R"topo(
namespace orders {
  public:
    slice<record<id: i64>> lookup(string key);
}
)topo";
    auto a = build(kBeforeOpt);
    auto b = build(kAfterSlice);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_FALSE(r.l1Pass)
        << "changing the container around a nested record (optional -> "
           "slice) is a real shape change and must still fail L1";
    EXPECT_FALSE(r.l1Differences.empty());
}

// ── L1: adding a declaration is rejected (shape changed) ────────────────

TEST(DualContractVerifier, AddedDeclarationFailsL1) {
    const char* afterExtra = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> record<id: i64, amount: f64>;
    handler persist(record<id: i64, amount: f64> o) -> bool;
    handler audit(bool ok) -> bool;
}
)topo";
    auto a = build(kBefore);
    auto b = build(afterExtra);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_FALSE(r.l1Pass);
    EXPECT_FALSE(r.l1Differences.empty());
}

// ── L1: a bridged rename matches under the rename map ───────────────────

TEST(DualContractVerifier, RenamedDeclarationMatchesViaRenameMap) {
    // The `persist` handler is renamed to `store`. Without the rename map
    // L1 sees a missing + an unexpected declaration; with the map it is a
    // clean rename.
    const char* afterRenamed = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> record<id: i64, amount: f64>;
    handler store(record<id: i64, amount: f64> o) -> bool;
}
)topo";
    auto a = build(kBefore);
    auto b = build(afterRenamed);

    DualContractVerifier v;

    // Without the rename map → L1 flags the mismatch.
    auto bare = v.verifyL1(a, b, {});
    EXPECT_FALSE(bare.l1Pass);

    // With the rename map → the rename is reconciled and L1 passes.
    auto mapped = v.verifyL1(a, b,
                             {{"orders::persist", "orders::store"}});
    EXPECT_TRUE(mapped.l1Pass)
        << (mapped.l1Differences.empty() ? "" : mapped.l1Differences[0]);
}

// ── L1: a parameter reorder (same multiset, different positions) is flagged ─

TEST(DualContractVerifier, ParameterReorderFailsL1) {
    // Same (name, type) multiset, different positions: positional type
    // comparison would pass (int == int at both slots), but the caller-
    // visible parameter order has flipped — a breaking change for every
    // positional call site. L1 must flag this as a potential reorder.
    const char* kReorderBefore = R"topo(
namespace orders {
  public:
    bool swap(i64 x, i64 y);
}
)topo";
    const char* kReorderAfter = R"topo(
namespace orders {
  public:
    bool swap(i64 y, i64 x);
}
)topo";
    auto a = build(kReorderBefore);
    auto b = build(kReorderAfter);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    EXPECT_FALSE(r.l1Pass);
    bool sawReorder = false;
    for (const auto& d : r.l1Differences) {
        if (d.find("parameter order changed") != std::string::npos) {
            sawReorder = true;
        }
    }
    EXPECT_TRUE(sawReorder)
        << "expected 'parameter order changed' diagnostic; got "
        << (r.l1Differences.empty() ? "no diffs" : r.l1Differences[0]);
}

TEST(DualContractVerifier, RenamedParameterIsNotAReorder) {
    // Different (name, type) multiset → not a reorder, so the "parameter
    // order changed" diagnostic must NOT fire (a pure name rename is a
    // distinct concern outside L1's structural scope).
    const char* kRenamedBefore = R"topo(
namespace orders {
  public:
    bool swap(i64 x, i64 y);
}
)topo";
    const char* kRenamedAfter = R"topo(
namespace orders {
  public:
    bool swap(i64 a, i64 b);
}
)topo";
    auto a = build(kRenamedBefore);
    auto b = build(kRenamedAfter);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {});
    // The structural shape (types, count) is preserved — L1 passes overall,
    // and crucially no spurious reorder diagnostic is emitted.
    for (const auto& d : r.l1Differences) {
        EXPECT_EQ(d.find("parameter order changed"), std::string::npos) << d;
    }
}

// ── L1: claim-tracking on the after side catches masked removals ────────

TEST(DualContractVerifier, RenameIntoExistingNameDoesNotMaskARemoval) {
    // before: parse, persist. after: only persist. A rename map that maps
    // parse -> persist tries to "rename parse into the already-existing
    // persist", which would mask that one declaration was actually removed
    // (two became one). Without claim-tracking on the new side this slips
    // through as a clean rename; with it, the collapsed count is a diff and
    // L1 must reject.
    const char* kTwo = R"topo(
namespace orders {
  public:
    handler parse(string raw) -> bool;
    handler persist(bool b) -> bool;
}
)topo";
    const char* kOne = R"topo(
namespace orders {
  public:
    handler persist(bool b) -> bool;
}
)topo";
    auto a = build(kTwo);
    auto b = build(kOne);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {{"orders::parse", "orders::persist"}});
    EXPECT_FALSE(r.l1Pass)
        << "renaming one declaration onto another that already exists "
           "collapses two into one — a masked removal L1 must reject";
    EXPECT_FALSE(r.l1Differences.empty());
}

TEST(DualContractVerifier, AfterSideSymbolWithNoOriginIsFlagged) {
    // An after-side declaration that no before-side declaration maps to
    // (even with an equal count maintained by an unrelated removal) must be
    // reported as "appeared after migration with no pre-migration origin" —
    // the claim-tracking leftover check on the new side.
    const char* kBeforeAB = R"topo(
namespace orders {
  public:
    handler alpha(bool b) -> bool;
    handler beta(bool b) -> bool;
}
)topo";
    // alpha is renamed to gamma; beta is dropped; delta is newly invented.
    // Count stays 2 -> 2, so only claim-tracking (not the count check) can
    // catch beta's removal and delta's invented appearance.
    const char* kAfterGD = R"topo(
namespace orders {
  public:
    handler gamma(bool b) -> bool;
    handler delta(bool b) -> bool;
}
)topo";
    auto a = build(kBeforeAB);
    auto b = build(kAfterGD);

    DualContractVerifier v;
    auto r = v.verifyL1(a, b, {{"orders::alpha", "orders::gamma"}});
    EXPECT_FALSE(r.l1Pass)
        << "beta removed + delta invented under an equal count must be "
           "caught by after-side claim tracking, not silently passed";
    bool sawAppeared = false, sawNoCounterpart = false;
    for (const auto& d : r.l1Differences) {
        if (d.find("appeared after migration") != std::string::npos)
            sawAppeared = true;
        if (d.find("no counterpart after migration") != std::string::npos)
            sawNoCounterpart = true;
    }
    EXPECT_TRUE(sawAppeared)
        << "delta (invented) must be flagged as appearing with no origin";
    EXPECT_TRUE(sawNoCounterpart)
        << "beta (removed) must be flagged as having no counterpart";
}

// ── L2 is a recorded MVP gap, not a silent skip ─────────────────────────

TEST(DualContractVerifier, L2IsUnavailableInThisMvp) {
    EXPECT_FALSE(DualContractVerifier::l2Available());
    // The reason string must be non-empty so a call site held back for L2
    // carries a precise reason in the migration report.
    EXPECT_NE(std::string(DualContractVerifier::l2UnavailableReason()), "");
}
