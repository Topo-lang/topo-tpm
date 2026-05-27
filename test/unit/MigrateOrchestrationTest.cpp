// In-process tests for `tpm migrate` orchestration semantics.
//
// MigrationEngineTest covers a single rule set applied to one source.
// This file covers the orchestration cmdMigrate wraps around the
// engine — specifically:
//   1. Dry-run must thread each step's rewrite forward in memory so the
//      preview reflects the chain (not each step seeing the on-disk
//      source as if it were the first step).
//   2. --apply must snapshot pre-migration on-disk contents and roll
//      every touched file back when a later step hard-fails, so a
//      crash mid-chain never leaves a half-migrated consumer tree.
//
// Issue: tpm-migrate-dry-run-and-rollback-gaps.
//
// Each test builds a minimal project: tpm.lock + .topo-pkgs/<pkg>/<ver>/
// (tpm.toml + migrations/index.toml + rule files) + one consumer .topo,
// then drives `tpm::cmdMigrate` directly. Stdout/stderr are captured to
// inspect orchestration output.

#include "tpm/Commands.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// Cross-platform replacement for POSIX mkdtemp(). MinGW does not ship
// mkdtemp; rather than gate the test out on Windows, build a unique
// directory under the OS temp root.
fs::path makeUniqueTempDir(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    fs::path candidate;
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::ostringstream name;
        name << prefix << '-' << now << '-' << seq << '-' << attempt;
        candidate = fs::temp_directory_path() / name.str();
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}
} // namespace

namespace {

class MigrateOrchestrationFixture : public ::testing::Test {
protected:
    fs::path tmp;

    void SetUp() override {
        tmp = makeUniqueTempDir("topo-tpm-migrate");
        ASSERT_FALSE(tmp.empty()) << "failed to create unique temp dir";
    }

    void TearDown() override {
        if (!tmp.empty()) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        }
    }

    void writeFile(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// Set up the minimal on-disk layout cmdMigrate expects:
    ///   <tmp>/
    ///     tpm.lock                   — locks pkg @ fromVersion
    ///     .topo-pkgs/<pkg>/<fromVersion>/
    ///       tpm.toml                 — declaration package manifest
    ///       migrations/
    ///         index.toml             — version-range → rule-file index
    ///         <rule files written by the caller>
    void seedProject(const std::string& pkg,
                     const std::string& fromVersion,
                     const std::string& indexToml) {
        // tpm.lock pinning the package at fromVersion. Lock::load
        // requires the full LockedPackage record (name/version/
        // source/revision/content_hash); the loader does not check
        // that the hash matches anything on disk for a migrate call.
        std::ostringstream lock;
        lock << "[[package]]\n"
             << "name = \"" << pkg << "\"\n"
             << "version = \"" << fromVersion << "\"\n"
             << "source = \"git+https://example.invalid/" << pkg
             << ".git#v" << fromVersion << "\"\n"
             << "revision = \"deadbeef\"\n"
             << "content_hash = \"sha256-test\"\n";
        writeFile(tmp / "tpm.lock", lock.str());

        fs::path pkgDir = tmp / ".topo-pkgs" / pkg / fromVersion;
        writeFile(pkgDir / "tpm.toml",
                  std::string("[package]\nname = \"") + pkg +
                  "\"\nversion = \"" + fromVersion +
                  "\"\nlicense = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
                  "kind = \"declaration\"\n");
        writeFile(pkgDir / "migrations" / "index.toml", indexToml);
    }

    /// Run `tpm::cmdMigrate(args)` with stdout+stderr captured.
    struct Captured {
        int exitCode = 0;
        std::string stdoutText;
        std::string stderrText;
    };
    Captured runMigrate(const std::vector<std::string>& args) {
        Captured c;
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();
        c.exitCode = tpm::cmdMigrate(args);
        c.stdoutText = ::testing::internal::GetCapturedStdout();
        c.stderrText = ::testing::internal::GetCapturedStderr();
        return c;
    }
};

// ── 1. dry-run threads cursor forward across steps ─────────────────

TEST_F(MigrateOrchestrationFixture, DryRunMultiStepPipelinesOutput) {
    // Two-step chain:
    //   step 0.2.0 -> 0.3.0  renames the field `id` to `order_id`
    //   step 0.3.0 -> 1.0.0  renames `order_id` to `external_order_id`
    //
    // With the in-memory cursor each step's rewrite feeds the next:
    //   step 1: id -> order_id  (preview includes one auto entry)
    //   step 2: order_id -> external_order_id  (preview also has an auto
    //                                           entry, because step 1's
    //                                           rewrite IS what step 2 sees)
    //
    // Without the cursor, step 2 would read the original on-disk source
    // (still carrying `id`, never `order_id`) and emit zero auto entries
    // for its rename, because no `order_id` exists in the disk text.
    // This test fails on the buggy code path.
    const std::string pkg = "topo/multistep-pkg";
    seedProject(pkg, "0.2.0",
                "[[migration]]\n"
                "from = \">=0.2.0, <0.3.0\"\n"
                "to   = \"0.3.0\"\n"
                "rules = \"step1.migration.toml\"\n"
                "[[migration]]\n"
                "from = \">=0.3.0, <1.0.0\"\n"
                "to   = \"1.0.0\"\n"
                "rules = \"step2.migration.toml\"\n");
    fs::path migDir = tmp / ".topo-pkgs" / pkg / "0.2.0" / "migrations";
    writeFile(migDir / "step1.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n"
              "  old = \"id\"\n"
              "  new = \"order_id\"\n");
    writeFile(migDir / "step2.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n"
              "  old = \"order_id\"\n"
              "  new = \"external_order_id\"\n");

    // Consumer: a record carrying `id` and an `orders::persist` handler
    // that takes it. The name-bridge rule is anchored to handler
    // `orders::persist` so every record reachable from it migrates.
    const std::string consumerSrc = R"topo(
namespace orders {
  public:
    handler persist(record<id: i64, amount: f64> o) -> bool;

    flow pipeline {
      persist -> void;
    }
}
)topo";
    fs::path consumerFile = tmp / "consumer.topo";
    writeFile(consumerFile, consumerSrc);

    auto r = runMigrate({"--dir", tmp.string(),
                         "--package", pkg,
                         "--to", "1.0.0"});
    EXPECT_EQ(r.exitCode, 0)
        << "dry-run on a clean auto-chain should exit 0\nstdout:\n"
        << r.stdoutText << "\nstderr:\n" << r.stderrText;

    // Dry-run never touches disk.
    EXPECT_EQ(readFile(consumerFile), consumerSrc)
        << "dry-run must not write the consumer file";

    // The output must announce both steps and a "would be rewritten"
    // preview line for each, proving step 2 saw step 1's output.
    EXPECT_NE(r.stdoutText.find("step -> 0.3.0"), std::string::npos)
        << "step 1 header missing:\n" << r.stdoutText;
    EXPECT_NE(r.stdoutText.find("step -> 1.0.0"), std::string::npos)
        << "step 2 header missing:\n" << r.stdoutText;

    // Count the "would be rewritten" preview lines. With the cursor
    // fix every step that auto-rewrites announces it; pre-fix step 2
    // produces no rewrite (it does not find `order_id` on disk).
    size_t pos = 0, wouldCount = 0;
    while ((pos = r.stdoutText.find("would be rewritten", pos)) !=
           std::string::npos) {
        ++wouldCount;
        ++pos;
    }
    EXPECT_EQ(wouldCount, 2u)
        << "dry-run must preview both steps' rewrites (got "
        << wouldCount << "):\n" << r.stdoutText;

    // Both steps' rules should have produced auto entries; if step 2
    // never finds `order_id` (the pre-fix bug) it emits no report row
    // at all for its rename, so the auto count drops.
    size_t autoCount = 0;
    pos = 0;
    while ((pos = r.stdoutText.find("[auto]", pos)) != std::string::npos) {
        ++autoCount;
        ++pos;
    }
    EXPECT_GE(autoCount, 2u)
        << "dry-run must show one auto entry per step (got "
        << autoCount << "):\n" << r.stdoutText;
}

// ── 2. --apply rolls back every written file on a mid-step hard error ──

TEST_F(MigrateOrchestrationFixture, ApplyRollsBackOnMidStepFailure) {
    // Two-step chain. Step 1 is a clean auto rename (writes the file).
    // Step 2 retypes a field to ">" — a single closing angle bracket —
    // which renders into the record-type slot and the lexer/parser
    // cannot reassemble it. MigrationEngine reports this as a hard
    // error ("no longer parses"); cmdMigrate sees `res.ok == false`
    // and must roll back step 1's write.
    const std::string pkg = "topo/rollback-pkg";
    seedProject(pkg, "0.2.0",
                "[[migration]]\n"
                "from = \">=0.2.0, <0.3.0\"\n"
                "to   = \"0.3.0\"\n"
                "rules = \"step1.migration.toml\"\n"
                "[[migration]]\n"
                "from = \">=0.3.0, <1.0.0\"\n"
                "to   = \"1.0.0\"\n"
                "rules = \"step2.migration.toml\"\n");
    fs::path migDir = tmp / ".topo-pkgs" / pkg / "0.2.0" / "migrations";
    writeFile(migDir / "step1.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n"
              "  old = \"id\"\n"
              "  new = \"order_id\"\n");
    // Step 2: retype `amount` to a single `>` — unparseable rewrite.
    writeFile(migDir / "step2.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.field_changes]]\n"
              "  op = \"retype\"\n"
              "  field = \"amount\"\n"
              "  type = \">\"\n");

    const std::string consumerSrc = R"topo(
namespace orders {
  public:
    handler persist(record<id: i64, amount: f64> o) -> bool;

    flow pipeline {
      persist -> void;
    }
}
)topo";
    fs::path consumerFile = tmp / "consumer.topo";
    writeFile(consumerFile, consumerSrc);

    auto r = runMigrate({"--dir", tmp.string(),
                         "--package", pkg,
                         "--to", "1.0.0",
                         "--apply"});
    // Always print the captured streams on failure so a regression
    // here doesn't force re-running with a debugger to see why.
    SCOPED_TRACE("migrate stdout:\n" + r.stdoutText +
                 "\nmigrate stderr:\n" + r.stderrText);

    EXPECT_EQ(r.exitCode, 1)
        << "step 2 hard error must surface as exit 1";
    EXPECT_NE(r.stderrText.find("rolled back"), std::string::npos)
        << "rollback must be announced on stderr";
    EXPECT_NE(r.stderrText.find("1 file(s)"), std::string::npos)
        << "rollback count should name the one file we touched";

    // The consumer file must be byte-identical to the pre-migration
    // source — step 1's intermediate write was rolled back.
    EXPECT_EQ(readFile(consumerFile), consumerSrc)
        << "consumer file should be restored to pre-migration text "
           "after rollback";
}

// ── 3. --apply that completes cleanly leaves the rewrite in place ──

TEST_F(MigrateOrchestrationFixture, ApplyChainCommitsFinalCursor) {
    // Sanity bookend for the rollback test: when no step hard-fails,
    // --apply must leave the FINAL cursor state on disk (i.e. step 2's
    // output, not step 1's), exercising the same cursor path as the
    // dry-run case.
    const std::string pkg = "topo/commit-pkg";
    seedProject(pkg, "0.2.0",
                "[[migration]]\n"
                "from = \">=0.2.0, <0.3.0\"\n"
                "to   = \"0.3.0\"\n"
                "rules = \"step1.migration.toml\"\n"
                "[[migration]]\n"
                "from = \">=0.3.0, <1.0.0\"\n"
                "to   = \"1.0.0\"\n"
                "rules = \"step2.migration.toml\"\n");
    fs::path migDir = tmp / ".topo-pkgs" / pkg / "0.2.0" / "migrations";
    writeFile(migDir / "step1.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n"
              "  old = \"id\"\n"
              "  new = \"order_id\"\n");
    writeFile(migDir / "step2.migration.toml",
              "[[rule]]\n"
              "kind = \"handler\"\n"
              "target = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n"
              "  old = \"order_id\"\n"
              "  new = \"external_order_id\"\n");

    const std::string consumerSrc = R"topo(
namespace orders {
  public:
    handler persist(record<id: i64, amount: f64> o) -> bool;

    flow pipeline {
      persist -> void;
    }
}
)topo";
    fs::path consumerFile = tmp / "consumer.topo";
    writeFile(consumerFile, consumerSrc);

    auto r = runMigrate({"--dir", tmp.string(),
                         "--package", pkg,
                         "--to", "1.0.0",
                         "--apply"});
    EXPECT_EQ(r.exitCode, 0)
        << "clean two-step --apply should exit 0\nstdout:\n"
        << r.stdoutText << "\nstderr:\n" << r.stderrText;

    std::string final_ = readFile(consumerFile);
    SCOPED_TRACE("migrate stdout:\n" + r.stdoutText +
                 "\nmigrate stderr:\n" + r.stderrText +
                 "\nfinal file:\n" + final_);

    EXPECT_NE(final_.find("external_order_id"), std::string::npos)
        << "final on-disk state must reflect step 2's rename";
    // Use `<external_order_id` / `<order_id` / `<id` as anchors so the
    // substring checks don't trip on the longer-name prefix.
    EXPECT_EQ(final_.find("<order_id:"), std::string::npos)
        << "intermediate step-1-only state (`order_id` field) must "
           "not survive into the final file";
    EXPECT_EQ(final_.find("<id:"), std::string::npos)
        << "original `id` field must not survive into the final file";
}

} // namespace
