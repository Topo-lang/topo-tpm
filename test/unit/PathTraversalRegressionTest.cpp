// Regression tests for path traversal via untrusted manifest fields.
//
// Three attack surfaces are pinned here:
//   1. SemVer prerelease string accepts ``..`` / ``/`` —
//      Manifest::validate now rejects it.
//   2. ``[[migration]].rules`` accepts arbitrary subpath / absolute
//      path — MigrationIndex::load now restricts it to a bare filename.
//   3. ``tpm install --from`` followed source symlinks during the
//      recursive copy — Commands.cpp now passes
//      ``fs::copy_options::copy_symlinks``. The copy-side defence is
//      indirectly verified by the install-from end-to-end test (not
//      reproduced here as a unit test because spawning ``git`` and a
//      full cache is out of scope for the unit tier).

#include "tpm/Manifest.h"
#include "tpm/MigrationRule.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace tpm;

namespace {
std::string fixture(const char* name) {
    return std::string(TPM_TEST_FIXTURES_DIR) + "/" + name;
}
} // namespace

// ── 1. version-field path traversal ─────────────────────────────────

TEST(PathTraversalRegression, MaliciousVersionRejected) {
    // Manifest::load itself succeeds — the TOML parses cleanly. The
    // reject is in validate(), the gate that callers MUST run before
    // composing any cache path.
    std::string err;
    auto m = Manifest::load(fixture("malicious-version-manifest.toml"), err);
    ASSERT_TRUE(m) << err;

    auto problems = m->validate();
    bool sawReject = std::any_of(
        problems.begin(), problems.end(), [](const std::string& s) {
            return s.find("path") != std::string::npos &&
                   s.find("version") != std::string::npos;
        });
    EXPECT_TRUE(sawReject)
        << "Manifest::validate must reject '1.0.0-../../../etc/passwd' as a "
           "path-traversal payload; got problems:\n  " +
               [&]() {
                   std::string j;
                   for (const auto& p : problems) j += p + "\n  ";
                   return j;
               }();
}

TEST(PathTraversalRegression, ForwardSlashInVersionRejected) {
    Manifest m;
    m.name = "acme/widget";
    m.version = "1.0.0-foo/bar";
    m.license = "MIT";
    m.coreCompat = "^1.0";
    auto problems = m.validate();
    bool sawReject = std::any_of(
        problems.begin(), problems.end(), [](const std::string& s) {
            return s.find("path separator") != std::string::npos;
        });
    EXPECT_TRUE(sawReject);
}

TEST(PathTraversalRegression, BackslashInVersionRejected) {
    Manifest m;
    m.name = "acme/widget";
    m.version = "1.0.0-foo\\bar";
    m.license = "MIT";
    m.coreCompat = "^1.0";
    auto problems = m.validate();
    bool sawReject = std::any_of(
        problems.begin(), problems.end(), [](const std::string& s) {
            return s.find("path separator") != std::string::npos;
        });
    EXPECT_TRUE(sawReject);
}

TEST(PathTraversalRegression, PlainSemverStillAccepted) {
    Manifest m;
    m.name = "acme/widget";
    m.version = "1.2.3";
    m.license = "MIT";
    m.coreCompat = "^1.0";
    auto problems = m.validate();
    // Other validators may complain about unrelated fields; we just
    // need the version-path check NOT to fire.
    bool sawPathReject = std::any_of(
        problems.begin(), problems.end(), [](const std::string& s) {
            return s.find("path") != std::string::npos &&
                   s.find("version") != std::string::npos;
        });
    EXPECT_FALSE(sawPathReject);
}

TEST(PathTraversalRegression, BenignPrereleaseStillAccepted) {
    // Non-path prerelease text remains accepted (SemVer's primary use).
    Manifest m;
    m.name = "acme/widget";
    m.version = "1.2.3-alpha.1";
    m.license = "MIT";
    m.coreCompat = "^1.0";
    auto problems = m.validate();
    bool sawPathReject = std::any_of(
        problems.begin(), problems.end(), [](const std::string& s) {
            return s.find("path") != std::string::npos &&
                   s.find("version") != std::string::npos;
        });
    EXPECT_FALSE(sawPathReject);
}

// ── 2. [[migration]].rules path traversal ────────────────────────────

TEST(PathTraversalRegression, MaliciousRulesFieldRejected) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("malicious-rules-index.toml"),
                                    exists, err);
    EXPECT_FALSE(idx)
        << "MigrationIndex::load must reject 'rules = \"../../../etc/passwd\"'";
    EXPECT_NE(err.find("rules"), std::string::npos);
    EXPECT_NE(err.find("bare filename"), std::string::npos);
}
