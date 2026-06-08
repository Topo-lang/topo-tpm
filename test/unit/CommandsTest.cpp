// In-process tests for tpm command orchestration that the engine-level
// and CLI-spawn suites do not reach. Each test drives a `tpm::cmd*`
// entry point directly against a scratch project under a unique temp
// directory and asserts on exit code, stdout/stderr, and on-disk state.
//
// Covers the deferred lower-severity command residuals:
//   - cmdInstall TOFU content-hash persistence into an existing tpm.lock
//   - install --from path-guard correctness under a RELATIVE --dir
//   - migrate --apply lock scope covering the --source directory
//   - cmdInit TOCTOU project-lock + idempotent existence guard
//   - cmdAdd stale-lock warning when resolution/install fails

#include "tpm/Commands.h"

#include "tpm/Cache.h"

#include "topo/Platform/FileLock.h"
#include "topo/Platform/Sanitize.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path makeUniqueTempDir(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::ostringstream name;
        name << prefix << '-' << now << '-' << seq << '-' << attempt;
        fs::path candidate = fs::temp_directory_path() / name.str();
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate;
    }
    return {};
}

class CommandsFixture : public ::testing::Test {
protected:
    fs::path tmp;
    fs::path savedCwd;

    void SetUp() override {
        savedCwd = fs::current_path();
        tmp = makeUniqueTempDir("topo-tpm-cmd");
        ASSERT_FALSE(tmp.empty()) << "failed to create temp dir";
    }
    void TearDown() override {
        std::error_code ec;
        fs::current_path(savedCwd, ec); // always restore cwd
        if (!tmp.empty()) fs::remove_all(tmp, ec);
    }

    void writeFile(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }
    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    struct Captured {
        int exitCode = 0;
        std::string out, err;
    };
    template <typename Fn>
    Captured run(Fn&& fn) {
        Captured c;
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();
        c.exitCode = fn();
        c.out = ::testing::internal::GetCapturedStdout();
        c.err = ::testing::internal::GetCapturedStderr();
        return c;
    }

    /// Build a minimal pre-packed package directory (a `tpm pack` output)
    /// that `tpm install --from` consumes. Declaration kind needs a
    /// non-empty declarations/ dir to verify, but install --from does not
    /// run verify, so a bare manifest is enough.
    fs::path seedPackage(const fs::path& at, const std::string& name,
                         const std::string& version) {
        writeFile(at / "tpm.toml",
                  "[package]\nname = \"" + name + "\"\nversion = \"" +
                  version + "\"\nlicense = \"MIT\"\n"
                  "core_compat = \">=0.1.0\"\nkind = \"declaration\"\n");
        writeFile(at / "declarations" / "api.topo",
                  "namespace x { public: handler h(bool b) -> bool; }\n");
        return at;
    }
};

// ── install --from path guard is correct under a RELATIVE --dir ─────────

TEST_F(CommandsFixture, InstallFromRelativeDirInstallsLegitPackage) {
    // The defence-in-depth containment check composes the cache-relative
    // `<name>/<version>` subpath, NOT the already-rooted `dest`. Before the
    // fix, a relative `--dir` (the default `.`) made it validate a doubled
    // `<root>/<root>/<name>/<version>` path — the wrong string. This test
    // chdirs into the scratch dir and installs with `--dir .` so the dest
    // and cache root are both relative — the exact shape that exercised the
    // doubled-path bug — and asserts the install completes and the package
    // lands at the correct cache path.
    fs::path pkg = seedPackage(tmp / "incoming", "topo/example-pkg", "1.0.0");
    fs::current_path(tmp); // make --dir "." relative to the scratch root

    auto c = run([&] {
        return tpm::cmdInstall({"--dir", ".", "--from", pkg.string()});
    });
    EXPECT_EQ(c.exitCode, 0)
        << "legit install under a relative --dir must succeed\nout:\n"
        << c.out << "\nerr:\n" << c.err;
    EXPECT_EQ(c.err.find("path-traversal"), std::string::npos)
        << "a legit package must not trip the path-traversal guard:\n"
        << c.err;

    fs::path cached =
        tmp / ".topo-pkgs" / "topo" / "example-pkg" / "1.0.0" / "tpm.toml";
    EXPECT_TRUE(fs::is_regular_file(cached))
        << "package must land at the real (non-doubled) cache path: "
        << cached;
    // The doubled path the buggy guard validated must NOT exist on disk.
    EXPECT_FALSE(fs::exists(tmp / ".topo-pkgs" / ".topo-pkgs"))
        << "the doubled .topo-pkgs/.topo-pkgs path must never be created";
}

TEST_F(CommandsFixture, InstallFromGuardChecksTheRealDestNotADoubledPath) {
    // Direct pin of the residual: with a RELATIVE --dir, the old guard
    // composed `sanitizePath(dest, cacheRoot)` where `dest` is already
    // rooted at cacheRoot, so sanitizePath joined cacheRoot onto it again
    // and validated a fictional `<root>/<root>/<name>/<version>` — the
    // wrong string. The fix passes the cache-relative `<name>/<version>`
    // subpath instead. This test reproduces both compositions and asserts
    // the corrected one resolves to the ACTUAL write target while the old
    // one resolves to the doubled path. Run from the scratch dir so the
    // cache root is relative, the exact bug trigger.
    fs::current_path(tmp);
    fs::create_directories(".topo-pkgs/topo/example-pkg/1.0.0");

    tpm::Cache cache(".");
    std::string dest = cache.packageDir("topo/example-pkg", "1.0.0");
    fs::path cacheRoot(cache.root());

    // OLD (buggy) composition: pass the already-rooted dest.
    auto oldResult = topo::platform::sanitizePath(fs::path(dest), cacheRoot);
    // NEW (fixed) composition: pass the cache-relative subpath.
    fs::path subpath = fs::path("topo/example-pkg") / "1.0.0";
    auto newResult = topo::platform::sanitizePath(subpath, cacheRoot);

    ASSERT_TRUE(newResult) << "the corrected guard must accept a legit pkg";
    // The corrected guard resolves to the real, singly-rooted dest.
    fs::path realDest = fs::weakly_canonical(fs::path(dest));
    EXPECT_EQ(*newResult, realDest)
        << "the fixed guard must validate the real write target " << realDest
        << ", got " << newResult->string();
    // The old composition resolved to a DIFFERENT (doubled) path — proof
    // the guard checked the wrong string before the fix.
    ASSERT_TRUE(oldResult);
    EXPECT_NE(*oldResult, realDest)
        << "the old composition must NOT equal the real dest (it validated "
           "a doubled .topo-pkgs/.topo-pkgs path); if these now match the "
           "test no longer pins the residual";
    // generic_string() so the forward-slash probe matches on Windows too
    // (native string() would use '\\' and never contain ".topo-pkgs/.topo-pkgs").
    EXPECT_NE(oldResult->generic_string().find(".topo-pkgs/.topo-pkgs"),
              std::string::npos)
        << "the old composition's resolved path should carry the doubled "
           "cache-root segment that proves the wrong-string bug; got "
        << oldResult->string();
}

// ── cmdInit takes a project lock and the existence guard is idempotent ──

TEST_F(CommandsFixture, InitCreatesManifestAndLockFile) {
    fs::path proj = tmp / "proj";
    auto c = run([&] {
        return tpm::cmdInit({"--dir", proj.string(), "--name", "acme/widget",
                             "--version", "0.1.0", "--kind", "declaration"});
    });
    EXPECT_EQ(c.exitCode, 0) << c.err;
    EXPECT_TRUE(fs::is_regular_file(proj / "tpm.toml"));
    // The project lock file is created as a side effect of taking the lock
    // that closes the init TOCTOU window.
    EXPECT_TRUE(fs::exists(proj / ".tpm-lock"))
        << "cmdInit must take the project lock (.tpm-lock) before the "
           "existence check to make check-then-write atomic";
}

TEST_F(CommandsFixture, InitOnExistingManifestIsRejected) {
    fs::path proj = tmp / "proj2";
    auto first = run([&] {
        return tpm::cmdInit({"--dir", proj.string(), "--name", "acme/w",
                             "--version", "0.1.0"});
    });
    ASSERT_EQ(first.exitCode, 0) << first.err;

    auto second = run([&] {
        return tpm::cmdInit({"--dir", proj.string(), "--name", "acme/w",
                             "--version", "0.2.0"});
    });
    EXPECT_NE(second.exitCode, 0)
        << "a second init on an existing tpm.toml must fail";
    EXPECT_NE(second.err.find("already exists"), std::string::npos)
        << second.err;
    // The first init's version must survive — the second must not clobber.
    EXPECT_NE(readFile(proj / "tpm.toml").find("0.1.0"), std::string::npos)
        << "the existing manifest must be left intact";
}

TEST_F(CommandsFixture, InitBlocksWhileProjectLockHeld) {
    // Hold the project lock from this thread, then confirm a concurrent
    // cmdInit blocks on lock acquisition until released — proving cmdInit
    // serialises through the same lock that closes its TOCTOU window.
    fs::path proj = tmp / "proj3";
    fs::create_directories(proj);
    topo::platform::FileLock held(proj / ".tpm-lock");
    ASSERT_TRUE(held.ok()) << held.error();
    ASSERT_TRUE(held.acquire()) << held.error();

    std::atomic<bool> done{false};
    std::thread t([&] {
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();
        tpm::cmdInit({"--dir", proj.string(), "--name", "acme/z",
                      "--version", "0.1.0"});
        (void)::testing::internal::GetCapturedStdout();
        (void)::testing::internal::GetCapturedStderr();
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_FALSE(done.load())
        << "cmdInit must block while the project lock is held by another "
           "owner (TOCTOU guard)";
    EXPECT_FALSE(fs::exists(proj / "tpm.toml"))
        << "cmdInit must not write the manifest before it holds the lock";

    held.release();
    t.join();
    EXPECT_TRUE(done.load());
    EXPECT_TRUE(fs::is_regular_file(proj / "tpm.toml"))
        << "cmdInit must complete once the lock is released";
}

// ── cmdInstall persists a first-seen TOFU content hash into an
//    EXISTING tpm.lock ─────────────────────────────────────────────────

TEST_F(CommandsFixture, InstallPersistsFreshContentHashIntoExistingLock) {
    // Project layout: a consumer with an existing tpm.lock that pins a
    // package WITHOUT a content_hash (the first-resolution state), and that
    // package already present in the cache. `tpm install` recomputes the
    // hash in installLocked; the residual was that with an existing lock the
    // freshly computed hash was discarded (only written when no lock
    // existed), so `tpm verify` kept skipping the integrity check. After the
    // fix install must rewrite the lock with the now-populated content_hash.
    fs::path proj = tmp / "consumer";
    fs::create_directories(proj);

    // Manifest with no dependencies (so resolveDependencies is not invoked
    // and we exercise the existing-lock branch of cmdInstall purely).
    writeFile(proj / "tpm.toml",
              "[package]\nname = \"acme/app\"\nversion = \"0.1.0\"\n"
              "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
              "kind = \"declaration\"\n");

    // A cached package with no source (already on disk → installLocked
    // takes the cached branch, recomputes the hash, fills the empty pin).
    fs::path cached =
        proj / ".topo-pkgs" / "topo" / "dep" / "1.0.0";
    writeFile(cached / "tpm.toml",
              "[package]\nname = \"topo/dep\"\nversion = \"1.0.0\"\n"
              "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
              "kind = \"declaration\"\n");

    // Existing lock pins the package but leaves content_hash empty.
    writeFile(proj / "tpm.lock",
              "[[package]]\nname = \"topo/dep\"\nversion = \"1.0.0\"\n"
              "source = \"\"\nrevision = \"\"\ncontent_hash = \"\"\n");

    auto c = run([&] {
        return tpm::cmdInstall({"--dir", proj.string()});
    });
    EXPECT_EQ(c.exitCode, 0) << "out:\n" << c.out << "\nerr:\n" << c.err;

    std::string lockAfter = readFile(proj / "tpm.lock");
    // The empty content_hash must have been replaced with a real digest.
    EXPECT_EQ(lockAfter.find("content_hash = \"\""), std::string::npos)
        << "install must persist the first-seen content hash into the "
           "existing lock, not discard it:\n"
        << lockAfter;
    EXPECT_NE(lockAfter.find("content_hash = \""), std::string::npos);
}

// ── migrate --apply locks the --source directory it writes to ───────────

TEST_F(CommandsFixture, MigrateApplyLocksTheSourceDirectory) {
    // `tpm migrate --apply` mutates consumer .topo under --source. When
    // --source differs from --dir, the serialising lock must cover --source
    // (where the writes land), not only --dir. We hold the --source lock
    // from this thread and confirm an --apply migrate run blocks until it is
    // released — proving the write directory is serialised.
    const std::string pkg = "topo/mpkg";
    fs::path dir = tmp / "proj";
    fs::path source = tmp / "srctree";
    fs::create_directories(dir);
    fs::create_directories(source);

    // tpm.lock under --dir pins the package at 0.2.0.
    writeFile(dir / "tpm.lock",
              "[[package]]\nname = \"" + pkg + "\"\nversion = \"0.2.0\"\n"
              "source = \"git+https://example.invalid/m.git#v0.2.0\"\n"
              "revision = \"deadbeef\"\ncontent_hash = \"sha256-x\"\n");
    // package migrations under --dir's cache.
    fs::path pkgDir = dir / ".topo-pkgs" / pkg / "0.2.0";
    writeFile(pkgDir / "tpm.toml",
              "[package]\nname = \"" + pkg + "\"\nversion = \"0.2.0\"\n"
              "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
              "kind = \"declaration\"\n");
    writeFile(pkgDir / "migrations" / "index.toml",
              "[[migration]]\nfrom = \">=0.2.0, <1.0.0\"\nto = \"1.0.0\"\n"
              "rules = \"step.migration.toml\"\n");
    writeFile(pkgDir / "migrations" / "step.migration.toml",
              "[[rule]]\nkind = \"handler\"\ntarget = \"orders::persist\"\n"
              "  [[rule.name_bridges]]\n  old = \"id\"\n  new = \"order_id\"\n");
    // consumer .topo lives under --source.
    writeFile(source / "consumer.topo",
              "namespace orders {\n public:\n  handler persist(record<id: "
              "i64, amount: f64> o) -> bool;\n}\n");

    // Hold the --source project lock.
    topo::platform::FileLock held(source / ".tpm-lock");
    ASSERT_TRUE(held.ok()) << held.error();
    ASSERT_TRUE(held.acquire()) << held.error();

    std::atomic<bool> done{false};
    std::thread t([&] {
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();
        tpm::cmdMigrate({"--dir", dir.string(), "--source", source.string(),
                         "--package", pkg, "--to", "1.0.0", "--apply"});
        (void)::testing::internal::GetCapturedStdout();
        (void)::testing::internal::GetCapturedStderr();
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(done.load())
        << "migrate --apply must block on the --source lock while it is "
           "held — the writes land under --source, so that is the dir the "
           "lock must cover";

    held.release();
    t.join();
    EXPECT_TRUE(done.load());
    // After release the migration ran and rewrote the consumer file.
    EXPECT_NE(readFile(source / "consumer.topo").find("order_id"),
              std::string::npos)
        << "the migration must complete once the --source lock is free";
}

// ── cmdAdd warns the lock is stale when resolution fails ────────────────

TEST_F(CommandsFixture, AddWarnsStaleLockWhenResolutionFails) {
    // `tpm add <pkg>` with no `--registry` makes resolveDependencies fail
    // ("no resolvable git source"). By then the manifest has already been
    // rewritten with the new dependency, so the on-disk tpm.lock no longer
    // matches the manifest. cmdAdd must surface that STALE-lock condition
    // explicitly — not merely say the lock was "left untouched", which
    // reads as "all fine". The residual: the failure left the project in a
    // manifest-ahead-of-lock state with no actionable warning.
    fs::path proj = tmp / "consumer";
    fs::create_directories(proj);
    writeFile(proj / "tpm.toml",
              "[package]\nname = \"acme/app\"\nversion = \"0.1.0\"\n"
              "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
              "kind = \"declaration\"\n");

    auto c = run([&] {
        // No --registry → resolution cannot find a git source → failure.
        return tpm::cmdAdd({"--dir", proj.string(), "some/dep@^1.0"});
    });
    EXPECT_NE(c.exitCode, 0)
        << "add with an unresolvable dependency must fail";
    // The manifest was mutated (the dependency was written before resolve).
    EXPECT_NE(readFile(proj / "tpm.toml").find("some/dep"), std::string::npos)
        << "the manifest gained the dependency before resolution failed";
    // And the user is warned the lock is now stale relative to that manifest.
    EXPECT_NE(c.err.find("STALE"), std::string::npos)
        << "cmdAdd must explicitly warn the lock is stale, not just say "
           "'left untouched':\n"
        << c.err;
    EXPECT_NE(c.err.find("some/dep"), std::string::npos)
        << "the stale-lock warning must name the affected dependency:\n"
        << c.err;
}

} // namespace
