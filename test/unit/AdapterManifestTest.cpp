// Unit tests for the [[adapters]] manifest section (package-format §1.2):
// parse, validate (well-formedness + declaration-bearing-kind gate), and
// TOML round-trip. Library-level — no subprocess.

#include "tpm/Manifest.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tpm;

namespace {

fs::path writeManifest(const std::string& name, const std::string& toml) {
    auto p = fs::temp_directory_path() / name;
    { std::ofstream(p) << toml; }
    return p;
}

bool hasProblemContaining(const std::vector<std::string>& ps,
                          const std::string& needle) {
    for (const auto& p : ps)
        if (p.find(needle) != std::string::npos) return true;
    return false;
}

Manifest declarationManifest() {
    Manifest m;
    m.name = "my-org/bridge";
    m.version = "0.1.0";
    m.license = "Apache-2.0";
    m.coreCompat = ">=4.0.0, <5.0.0";
    m.kind = PackageKind::Declaration;
    return m;
}

} // namespace

TEST(AdapterManifest, ParsesAdaptersSection) {
    auto p = writeManifest("tpm-adapters-parse.toml", R"toml(
[package]
name = "my-org/bridge"
version = "0.1.0"
kind = "declaration"
license = "Apache-2.0"
core_compat = ">=4.0.0, <5.0.0"

[[adapters]]
from_library = "fmt"
to_library = "std-format"
languages = ["cpp"]

[[adapters]]
from_library = "boost-json"
to_library = "nlohmann-json"
languages = ["cpp", "rust"]
)toml");
    std::string err;
    auto m = Manifest::load(p.string(), err);
    ASSERT_TRUE(m) << err;
    ASSERT_EQ(m->adapters.size(), 2u);
    EXPECT_EQ(m->adapters[0].fromLibrary, "fmt");
    EXPECT_EQ(m->adapters[0].toLibrary, "std-format");
    ASSERT_EQ(m->adapters[0].languages.size(), 1u);
    EXPECT_EQ(m->adapters[0].languages[0], "cpp");
    EXPECT_EQ(m->adapters[1].fromLibrary, "boost-json");
    ASSERT_EQ(m->adapters[1].languages.size(), 2u);
    EXPECT_EQ(m->adapters[1].languages[1], "rust");
    EXPECT_TRUE(m->validate().empty()) << "a well-formed adapter set must pass";
    fs::remove(p);
}

TEST(AdapterManifest, AbsentSectionIsValidAndEmpty) {
    // A package without [[adapters]] parses with an empty adapter list and
    // stays valid (regression: the addition is optional).
    auto p = writeManifest("tpm-adapters-absent.toml", R"toml(
[package]
name = "my-org/plain"
version = "0.1.0"
kind = "declaration"
license = "MIT"
core_compat = ">=4.0.0, <5.0.0"
)toml");
    std::string err;
    auto m = Manifest::load(p.string(), err);
    ASSERT_TRUE(m) << err;
    EXPECT_TRUE(m->adapters.empty());
    EXPECT_TRUE(m->validate().empty());
    fs::remove(p);
}

TEST(AdapterManifest, ValidatesGoodDeclarationKind) {
    Manifest m = declarationManifest();
    m.adapters.push_back({"fmt", "std-format", {"cpp"}});
    EXPECT_TRUE(m.validate().empty());
}

TEST(AdapterManifest, AcceptedOnStdlibTypeAndKernel) {
    for (PackageKind k : {PackageKind::StdlibType, PackageKind::Kernel}) {
        EXPECT_TRUE(isDeclarationBearingKind(k));
        Manifest m = declarationManifest();
        m.kind = k;
        m.adapters.push_back({"fmt", "std-format", {"cpp"}});
        // The adapter rule itself must not be flagged for these kinds (other
        // kind-specific problems are out of scope for this assertion).
        EXPECT_FALSE(hasProblemContaining(m.validate(), "declaration-bearing"))
            << "kind " << kindToString(k) << " should accept [[adapters]]";
    }
}

TEST(AdapterManifest, RejectsAdaptersOnLayoutAndEventProtocol) {
    for (PackageKind k : {PackageKind::Layout, PackageKind::EventProtocol}) {
        EXPECT_FALSE(isDeclarationBearingKind(k));
        Manifest m = declarationManifest();
        m.kind = k;
        m.adapters.push_back({"fmt", "std-format", {"cpp"}});
        EXPECT_TRUE(hasProblemContaining(m.validate(), "declaration-bearing"))
            << "[[adapters]] on kind " << kindToString(k) << " must be rejected";
    }
}

TEST(AdapterManifest, RejectsEmptyLibraryAndBadLanguage) {
    Manifest m = declarationManifest();
    m.adapters.push_back({"", "std-format", {"cpp"}});  // empty from_library
    m.adapters.push_back({"fmt", "std-format", {}});     // no languages
    m.adapters.push_back({"a", "b", {"cobol"}});         // unknown language
    auto problems = m.validate();
    EXPECT_TRUE(hasProblemContaining(problems, "from_library"));
    EXPECT_TRUE(hasProblemContaining(problems, "no 'languages'"));
    EXPECT_TRUE(hasProblemContaining(problems, "invalid language 'cobol'"));
}

TEST(AdapterManifest, ToTomlRoundTrips) {
    Manifest m = declarationManifest();
    m.adapters.push_back({"fmt", "std-format", {"cpp"}});
    m.adapters.push_back({"boost-json", "nlohmann-json", {"cpp", "rust"}});

    auto p = writeManifest("tpm-adapters-roundtrip.toml", m.toToml());
    std::string err;
    auto reloaded = Manifest::load(p.string(), err);
    ASSERT_TRUE(reloaded) << err;
    ASSERT_EQ(reloaded->adapters.size(), 2u);
    EXPECT_EQ(reloaded->adapters[0].fromLibrary, "fmt");
    EXPECT_EQ(reloaded->adapters[0].toLibrary, "std-format");
    EXPECT_EQ(reloaded->adapters[0].languages,
              (std::vector<std::string>{"cpp"}));
    EXPECT_EQ(reloaded->adapters[1].languages,
              (std::vector<std::string>{"cpp", "rust"}));
    EXPECT_TRUE(reloaded->validate().empty());
    fs::remove(p);
}
