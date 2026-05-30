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

// ── L2 is a recorded MVP gap, not a silent skip ─────────────────────────

TEST(DualContractVerifier, L2IsUnavailableInThisMvp) {
    EXPECT_FALSE(DualContractVerifier::l2Available());
    // The reason string must be non-empty so a call site held back for L2
    // carries a precise reason in the migration report.
    EXPECT_NE(std::string(DualContractVerifier::l2UnavailableReason()), "");
}
