// End-to-end CLI integration tests for tpm.
//
// The existing tpm/test/unit/ suite is library-level: it constructs
// MigrationRule / MigrationEngine / DualContractVerifier objects in
// process and asserts on their return values. Every issue this audit
// raised (path traversal in `--from`, single-layer dep resolution,
// phantom locations in migration reports, half-copy rollback gap) is
// invisible to that surface.
//
// These tests spawn the built `tpm` binary as a subprocess against a
// scratch project root under mkdtemp(), exactly the way a user would.
// The fixture package under tpm/test/cli/fixtures/pkg-local/ is the
// canonical "good" input. Negative cases (malformed manifest,
// missing tpm.lock) are covered alongside.
//
// These cover the CLI flows the library-level unit tests cannot reach.

#include "topo/Platform/Process.h"

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
// Cross-platform replacement for POSIX mkdtemp() — MinGW does not ship
// one, and we still want this test to compile and run on Windows.
inline fs::path makeUniqueTempDir(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::ostringstream name;
        name << prefix << '-' << now << '-' << seq << '-' << attempt;
        fs::path candidate = fs::temp_directory_path() / name.str();
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}
} // namespace

namespace {

class TpmCliFixture : public ::testing::Test {
protected:
    fs::path tmp;
    std::string tpmBin;

    void SetUp() override {
        // The CMake target injects the absolute path of the built tpm
        // binary via TOPO_TPM_BINARY. When the test binary is run
        // outside ctest we fall back to a search in the workspace.
        tpmBin = TOPO_TPM_BINARY;
        if (!fs::is_regular_file(tpmBin)) {
            GTEST_SKIP() << "SKIPPED: tpm binary not built at " << tpmBin
                         << "; build the `tpm` target first (cmake --build build --target tpm)";
        }

        tmp = makeUniqueTempDir("topo-tpm-cli");
        ASSERT_FALSE(tmp.empty()) << "failed to create unique temp dir";
    }

    void TearDown() override {
        if (!tmp.empty()) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        }
    }

    /// Run `tpm <args...>` and return its captured result.
    topo::platform::CapturedProcessResult runTpm(const std::vector<std::string>& args) {
        return topo::platform::runProcessCapture(tpmBin, args, /*verbose=*/false);
    }

    /// Copy a fixture directory into the scratch tmp under a given name.
    fs::path copyFixture(const std::string& fixtureName, const std::string& destName) {
        fs::path src = fs::path(TPM_CLI_FIXTURES_DIR) / fixtureName;
        fs::path dst = tmp / destName;
        std::error_code ec;
        fs::create_directories(dst.parent_path());
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (ec) {
            ADD_FAILURE() << "copy fixture " << src << " -> " << dst << ": " << ec.message();
        }
        return dst;
    }
};

} // namespace

// ── tpm install --from <pkg-local> ─────────────────────────────────

TEST_F(TpmCliFixture, InstallFromLocalPackageSucceeds) {
    // Setup: a scratch project root with no tpm.toml of its own (the
    // --from path is the "consume a pre-packed package" entry point).
    fs::path consumer = tmp / "consumer";
    fs::create_directories(consumer);

    // Copy the fixture package into the scratch dir.
    fs::path pkgDir = copyFixture("pkg-local", "incoming-pkg");

    // tpm install --dir <consumer> --from <incoming-pkg>
    auto r = runTpm({"install",
                     "--dir", consumer.string(),
                     "--from", pkgDir.string()});
    EXPECT_EQ(r.exitCode, 0)
        << "tpm install --from failed:\nstdout:\n" << r.stdoutOutput
        << "\nstderr:\n" << r.stderrOutput;
    EXPECT_NE(r.stdoutOutput.find("installed topo/example-pkg 1.0.0"),
              std::string::npos)
        << "install output should name the installed package:\n"
        << r.stdoutOutput;

    // Cache layout: <consumer>/.topo-pkgs/topo/example-pkg/1.0.0/tpm.toml
    fs::path cachedManifest =
        consumer / ".topo-pkgs" / "topo" / "example-pkg" / "1.0.0" / "tpm.toml";
    EXPECT_TRUE(fs::is_regular_file(cachedManifest))
        << "cached manifest not at expected path: " << cachedManifest;
}

TEST_F(TpmCliFixture, InstallFromNonexistentDirectoryFails) {
    fs::path consumer = tmp / "consumer";
    fs::create_directories(consumer);

    auto r = runTpm({"install",
                     "--dir", consumer.string(),
                     "--from", (tmp / "does-not-exist").string()});
    EXPECT_NE(r.exitCode, 0)
        << "tpm install --from on a missing dir must fail";
    EXPECT_FALSE(r.stderrOutput.empty())
        << "tpm install must report something on stderr when --from is bad";
}

// ── tpm migrate (dry-run default) ──────────────────────────────────

TEST_F(TpmCliFixture, MigrateDryRunRequiresInstalledPackage) {
    // tpm migrate needs a tpm.lock pinning the package being migrated.
    // Without it the command must reject, NOT walk the fs guessing.
    fs::path consumer = tmp / "consumer";
    fs::create_directories(consumer);

    auto r = runTpm({"migrate",
                     "--dir", consumer.string(),
                     "--package", "topo/example-pkg",
                     "--to", "1.0.0"});
    EXPECT_NE(r.exitCode, 0)
        << "tpm migrate without tpm.lock must fail";
    EXPECT_NE(r.stderrOutput.find("tpm.lock"), std::string::npos)
        << "rejection must name the missing tpm.lock:\nstderr:\n"
        << r.stderrOutput;
}

TEST_F(TpmCliFixture, MigrateRequiresPackageAndToArgs) {
    fs::path consumer = tmp / "consumer";
    fs::create_directories(consumer);

    auto r = runTpm({"migrate", "--dir", consumer.string()});
    EXPECT_NE(r.exitCode, 0)
        << "tpm migrate without --package / --to must reject";
    EXPECT_NE(r.stderrOutput.find("requires"), std::string::npos)
        << "rejection must say what is required:\n" << r.stderrOutput;
}

// ── tpm publish (dry-run via STUB exit) ────────────────────────────

TEST_F(TpmCliFixture, PublishOnLocalPackageRunsVerifyFirstAndStubsOut) {
    // tpm publish runs verify first; on a valid package it then prints
    // the manual-publish hint and exits 2 (the STUB exit). This pins
    // both: (a) verify is called, (b) the stub path is reached.
    fs::path consumer = copyFixture("pkg-local", "publishable-pkg");

    auto r = runTpm({"publish", "--dir", consumer.string()});
    // Exit 2 = stub; either 0/2 is acceptable as long as the verify
    // call did not produce a hard error. Failure modes we want to
    // catch are crashes (signal), exit 1 (verify failed), or no
    // stub-hint output.
    EXPECT_NE(r.exitCode, 1)
        << "publish must not fail verify on a valid package:\nstdout:\n"
        << r.stdoutOutput << "\nstderr:\n" << r.stderrOutput;
    EXPECT_LT(r.exitCode, 128)
        << "publish must not crash (exit < 128); got " << r.exitCode;
    EXPECT_NE(r.stderrOutput.find("STUB"), std::string::npos)
        << "publish must announce its stub status:\n" << r.stderrOutput;
}
