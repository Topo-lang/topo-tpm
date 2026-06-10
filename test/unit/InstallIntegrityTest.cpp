// Trust-model regression tests for install:
//   - the locked `revision` pin is ENFORCED — a tag re-pointed between
//     resolution and install fails loudly instead of installing silently;
//   - a manifest-pinned `content_hash` turns the FIRST install from
//     trust-on-first-use into verify-against-publisher-intent.
//
// Each test builds a real git "registry" repo under a temp dir and drives
// `tpm::cmdInstall` against a consumer project via a file:// URL.

#include "tpm/Commands.h"
#include "tpm/GitRegistry.h"

#include "topo/Platform/Process.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

class InstallIntegrityFixture : public ::testing::Test {
protected:
    fs::path tmp;
    fs::path savedCwd;

    void SetUp() override {
        if (!tpm::GitRegistry::gitAvailable())
            GTEST_SKIP() << "git not on PATH — registry fixtures need it";
        savedCwd = fs::current_path();
        tmp = makeUniqueTempDir("topo-tpm-integrity");
        ASSERT_FALSE(tmp.empty()) << "failed to create temp dir";
    }
    void TearDown() override {
        std::error_code ec;
        fs::current_path(savedCwd, ec);
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

    void git(const std::vector<std::string>& args) {
        auto r = topo::platform::runProcessCapture("git", args);
        ASSERT_EQ(r.exitCode, 0)
            << "git failed: " << r.stderrOutput;
    }

    /// Real git repo serving as the registry: one package manifest,
    /// committed and tagged v1.0.0. Identity is configured repo-locally so
    /// commits work on bare CI runners.
    fs::path makeRegistryRepo() {
        fs::path repo = tmp / "registry-pkg";
        writeFile(repo / "tpm.toml",
                  "[package]\nname = \"topo/dep\"\nversion = \"1.0.0\"\n"
                  "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
                  "kind = \"declaration\"\n");
        writeFile(repo / "declarations" / "api.topo",
                  "namespace x { public: handler h(bool b) -> bool; }\n");
        git({"-C", repo.string(), "init", "-q", "-b", "main"});
        git({"-C", repo.string(), "config", "user.email", "test@topo"});
        git({"-C", repo.string(), "config", "user.name", "topo-test"});
        git({"-C", repo.string(), "add", "."});
        git({"-C", repo.string(), "commit", "-q", "-m", "v1"});
        git({"-C", repo.string(), "tag", "v1.0.0"});
        return repo;
    }

    static std::string fileUrl(const fs::path& p) {
        std::string s = p.generic_string();
        if (!s.empty() && s.front() != '/') s.insert(s.begin(), '/'); // win drive
        return "file://" + s;
    }

    /// Consumer project whose manifest depends on topo/dep from `repoUrl`.
    fs::path makeConsumer(const std::string& repoUrl,
                          const std::string& extraDepKeys = "") {
        fs::path proj = tmp / "consumer";
        fs::create_directories(proj);
        writeFile(proj / "tpm.toml",
                  "[package]\nname = \"acme/app\"\nversion = \"0.1.0\"\n"
                  "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
                  "kind = \"declaration\"\n\n[dependencies]\n"
                  "\"topo/dep\" = { version = \"^1.0.0\", registry = \"" +
                  repoUrl + "\"" + extraDepKeys + " }\n");
        return proj;
    }

    struct Captured {
        int exitCode = 0;
        std::string out, err;
    };
    Captured install(const fs::path& proj) {
        Captured c;
        ::testing::internal::CaptureStdout();
        ::testing::internal::CaptureStderr();
        c.exitCode = tpm::cmdInstall({"--dir", proj.string()});
        c.out = ::testing::internal::GetCapturedStdout();
        c.err = ::testing::internal::GetCapturedStderr();
        return c;
    }
};

} // namespace

// A tag re-pointed AFTER the lock recorded its revision delivers different
// content under the same name: install must fail naming the mismatch, and
// must not leave the unverified clone in the cache.
TEST_F(InstallIntegrityFixture, RepointedTagFailsAgainstLockedRevision) {
    fs::path repo = makeRegistryRepo();
    fs::path proj = makeConsumer(fileUrl(repo));

    auto first = install(proj);
    ASSERT_EQ(first.exitCode, 0)
        << "out:\n" << first.out << "\nerr:\n" << first.err;
    std::string lock = readFile(proj / "tpm.lock");
    ASSERT_NE(lock.find("revision = \""), std::string::npos) << lock;

    // Re-point v1.0.0 at a new commit (the attack / accidental force-push).
    writeFile(repo / "declarations" / "evil.topo", "namespace evil {}\n");
    git({"-C", repo.string(), "add", "."});
    git({"-C", repo.string(), "commit", "-q", "-m", "v1-repointed"});
    git({"-C", repo.string(), "tag", "-f", "v1.0.0"});

    // Drop the cache so install re-fetches; the lock (with the old
    // revision) stays.
    std::error_code ec;
    fs::remove_all(proj / ".topo-pkgs", ec);

    auto second = install(proj);
    EXPECT_NE(second.exitCode, 0)
        << "a re-pointed tag must not install silently:\nout:\n"
        << second.out << "\nerr:\n" << second.err;
    EXPECT_NE(second.err.find("revision mismatch"), std::string::npos)
        << second.err;
    EXPECT_FALSE(fs::exists(proj / ".topo-pkgs" / "topo" / "dep" / "1.0.0" /
                            "tpm.toml"))
        << "unverified content must not stay in the cache";
}

// A manifest-pinned content_hash makes the FIRST install verify against
// publisher intent: a wrong pin fails before anything is trusted.
TEST_F(InstallIntegrityFixture, ManifestPinnedHashMismatchFailsFirstInstall) {
    fs::path repo = makeRegistryRepo();
    fs::path proj = makeConsumer(fileUrl(repo),
                                 ", content_hash = \"deadbeef\"");

    auto c = install(proj);
    EXPECT_NE(c.exitCode, 0)
        << "a wrong publisher pin must fail the first install:\nout:\n"
        << c.out << "\nerr:\n" << c.err;
    EXPECT_NE(c.err.find("content hash mismatch"), std::string::npos)
        << c.err;
}

// And the positive half: pinning the TRUE hash succeeds on a pristine
// first install. The true value is learned from a TOFU install's lock, then
// everything (lock + cache) is wiped so the pinned run is genuinely first.
TEST_F(InstallIntegrityFixture, ManifestPinnedHashMatchPassesFirstInstall) {
    fs::path repo = makeRegistryRepo();
    fs::path proj = makeConsumer(fileUrl(repo));

    auto learn = install(proj);
    ASSERT_EQ(learn.exitCode, 0) << learn.err;
    std::string lock = readFile(proj / "tpm.lock");
    auto pos = lock.find("content_hash = \"");
    ASSERT_NE(pos, std::string::npos) << lock;
    pos += std::string("content_hash = \"").size();
    std::string trueHash = lock.substr(pos, lock.find('"', pos) - pos);
    ASSERT_FALSE(trueHash.empty());

    std::error_code ec;
    fs::remove_all(proj / ".topo-pkgs", ec);
    fs::remove(proj / "tpm.lock", ec);
    fs::path proj2 = proj; // same dir, rewritten manifest with the pin
    writeFile(proj2 / "tpm.toml",
              "[package]\nname = \"acme/app\"\nversion = \"0.1.0\"\n"
              "license = \"MIT\"\ncore_compat = \">=0.1.0\"\n"
              "kind = \"declaration\"\n\n[dependencies]\n"
              "\"topo/dep\" = { version = \"^1.0.0\", registry = \"" +
              fileUrl(repo) + "\", content_hash = \"" + trueHash + "\" }\n");

    auto pinned = install(proj2);
    EXPECT_EQ(pinned.exitCode, 0)
        << "the true pin must pass a pristine first install:\nout:\n"
        << pinned.out << "\nerr:\n" << pinned.err;
}
