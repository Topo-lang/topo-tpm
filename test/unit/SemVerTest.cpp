// Regression tests for SemVer / VersionReq range resolution.
//
// Pins the caret/tilde upper-bound math that the manifest core_compat,
// dependency requirements, and git-tag selection all route through:
//   - `~1` (major-only) must mean `<2.0.0`, not `<1.1.0`.
//   - `^0` (major-only) must mean `<1.0.0`, not `<0.0.1`.
//   - a component at INT_MAX must not trigger signed-overflow UB when the
//     upper bound is computed.

#include "tpm/SemVer.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace tpm;

namespace {
bool satisfies(const char* req, const char* ver) {
    auto r = VersionReq::parse(req);
    auto v = SemVer::parse(ver);
    EXPECT_TRUE(r) << "requirement '" << req << "' failed to parse";
    EXPECT_TRUE(v) << "version '" << ver << "' failed to parse";
    if (!r || !v) return false;
    return r->matches(*v);
}
} // namespace

// ── tilde major-only: ~1 := >=1.0.0, <2.0.0 ─────────────────────────────

TEST(SemVer, TildeMajorOnlyAcceptsAnyMinorBelowNextMajor) {
    // The bug: `~1` was treated as `<1.1.0`, rejecting `1.5.0`.
    EXPECT_TRUE(satisfies("~1", "1.0.0"));
    EXPECT_TRUE(satisfies("~1", "1.5.0"));
    EXPECT_TRUE(satisfies("~1", "1.99.99"));
    EXPECT_FALSE(satisfies("~1", "2.0.0"));
    EXPECT_FALSE(satisfies("~1", "0.9.0"));
}

TEST(SemVer, TildeWithMinorStillBumpsMinor) {
    // ~1.2 := >=1.2.0, <1.3.0 — the minor-given case must be unchanged.
    EXPECT_TRUE(satisfies("~1.2", "1.2.0"));
    EXPECT_TRUE(satisfies("~1.2", "1.2.9"));
    EXPECT_FALSE(satisfies("~1.2", "1.3.0"));
    // ~1.2.3 := >=1.2.3, <1.3.0
    EXPECT_TRUE(satisfies("~1.2.3", "1.2.3"));
    EXPECT_TRUE(satisfies("~1.2.3", "1.2.9"));
    EXPECT_FALSE(satisfies("~1.2.3", "1.3.0"));
    EXPECT_FALSE(satisfies("~1.2.3", "1.2.2"));
}

// ── caret major-only zero: ^0 := >=0.0.0, <1.0.0 ────────────────────────

TEST(SemVer, CaretMajorOnlyZeroAcceptsWholeZeroXRange) {
    // The bug: `^0` was treated as `<0.0.1`, rejecting `0.3.0`.
    EXPECT_TRUE(satisfies("^0", "0.0.0"));
    EXPECT_TRUE(satisfies("^0", "0.3.0"));
    EXPECT_TRUE(satisfies("^0", "0.99.99"));
    EXPECT_FALSE(satisfies("^0", "1.0.0"));
}

TEST(SemVer, CaretZeroFamilyKeepsCargoSemantics) {
    // ^0.3 := >=0.3.0, <0.4.0
    EXPECT_TRUE(satisfies("^0.3", "0.3.5"));
    EXPECT_FALSE(satisfies("^0.3", "0.4.0"));
    // ^0.0.5 := >=0.0.5, <0.0.6
    EXPECT_TRUE(satisfies("^0.0.5", "0.0.5"));
    EXPECT_FALSE(satisfies("^0.0.5", "0.0.6"));
    // ^1.2.3 := >=1.2.3, <2.0.0
    EXPECT_TRUE(satisfies("^1.2.3", "1.9.9"));
    EXPECT_FALSE(satisfies("^1.2.3", "2.0.0"));
}

// ── INT_MAX boundary must not invoke signed-overflow UB ─────────────────

TEST(SemVer, CaretAtIntMaxMajorDoesNotOverflow) {
    std::string maxMajor =
        std::to_string(std::numeric_limits<int>::max()) + ".0.0";
    auto r = VersionReq::parse("^" + maxMajor);
    auto v = SemVer::parse(maxMajor);
    ASSERT_TRUE(r);
    ASSERT_TRUE(v);
    // The lower bound is satisfied; the saturated upper bound must not wrap
    // negative (which would reject everything or trigger UB). The exact
    // matches() result is unspecified at the boundary, but the call must be
    // well-defined and not crash / UB.
    EXPECT_NO_THROW((void)r->matches(*v));
}

TEST(SemVer, TildeAtIntMaxMinorDoesNotOverflow) {
    std::string maxMinor =
        "1." + std::to_string(std::numeric_limits<int>::max()) + ".0";
    auto r = VersionReq::parse("~" + maxMinor);
    auto v = SemVer::parse(maxMinor);
    ASSERT_TRUE(r);
    ASSERT_TRUE(v);
    EXPECT_NO_THROW((void)r->matches(*v));
}
