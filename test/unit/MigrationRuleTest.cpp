// Tests for migration-rule parsing and version-range path selection
// (declaration-migration spec §4).

#include "tpm/MigrationRule.h"

#include <gtest/gtest.h>

#include <string>

using namespace tpm;

namespace {
std::string fixture(const char* name) {
    return std::string(TPM_TEST_FIXTURES_DIR) + "/" + name;
}
} // namespace

// ── *.migration.toml parsing (§4.3) ─────────────────────────────────────

TEST(MigrationRule, ParsesHandlerRuleWithFieldChanges) {
    std::string err;
    auto set = MigrationRuleSet::load(fixture("0.2.0-to-0.3.0.migration.toml"),
                                      err);
    ASSERT_TRUE(set) << err;
    ASSERT_EQ(set->rules.size(), 3u);

    const MigrationRule& handler = set->rules[0];
    EXPECT_EQ(handler.kind, MigrationKind::Handler);
    EXPECT_EQ(handler.target, "orders::validate");
    ASSERT_EQ(handler.fieldChanges.size(), 2u);

    EXPECT_EQ(handler.fieldChanges[0].op, FieldChange::Op::Add);
    EXPECT_EQ(handler.fieldChanges[0].field, "region");
    EXPECT_EQ(handler.fieldChanges[0].type, "i64");
    ASSERT_TRUE(handler.fieldChanges[0].defaultValue);
    EXPECT_EQ(*handler.fieldChanges[0].defaultValue, "0");

    EXPECT_EQ(handler.fieldChanges[1].op, FieldChange::Op::Retype);
    EXPECT_EQ(handler.fieldChanges[1].field, "amount");
}

TEST(MigrationRule, ParsesOperationFnAndPipelineFlowRules) {
    std::string err;
    auto set = MigrationRuleSet::load(fixture("0.2.0-to-0.3.0.migration.toml"),
                                      err);
    ASSERT_TRUE(set) << err;

    const MigrationRule& opFn = set->rules[1];
    EXPECT_EQ(opFn.kind, MigrationKind::OperationFn);
    ASSERT_EQ(opFn.paramChanges.size(), 1u);
    EXPECT_EQ(opFn.paramChanges[0].op, ParamChange::Op::Insert);
    EXPECT_EQ(opFn.paramChanges[0].position, 1);
    EXPECT_EQ(opFn.paramChanges[0].name, "cfg");

    const MigrationRule& flow = set->rules[2];
    EXPECT_EQ(flow.kind, MigrationKind::PipelineFlow);
    ASSERT_EQ(flow.edgeChanges.size(), 1u);
    EXPECT_EQ(flow.edgeChanges[0].op, EdgeChange::Op::Insert);
    EXPECT_EQ(flow.edgeChanges[0].from, "validate");
    EXPECT_EQ(flow.edgeChanges[0].to, "audit");
}

TEST(MigrationRule, ParsesNameBridge) {
    std::string err;
    auto set = MigrationRuleSet::load(fixture("0.3.0-to-1.0.0.migration.toml"),
                                      err);
    ASSERT_TRUE(set) << err;
    ASSERT_EQ(set->rules.size(), 1u);
    ASSERT_EQ(set->rules[0].nameBridges.size(), 1u);
    EXPECT_EQ(set->rules[0].nameBridges[0].oldName, "id");
    EXPECT_EQ(set->rules[0].nameBridges[0].newName, "order_id");
}

TEST(MigrationRule, KindParsingRejectsUnknownValues) {
    EXPECT_TRUE(parseMigrationKind("operation-fn"));
    EXPECT_TRUE(parseMigrationKind("handler"));
    EXPECT_TRUE(parseMigrationKind("pipeline-flow"));
    EXPECT_EQ(parseMigrationKind("nonsense"), std::nullopt);
}

TEST(MigrationRule, RuleIdCombinesKindAndTarget) {
    MigrationRule r;
    r.kind = MigrationKind::Handler;
    r.target = "orders::validate";
    EXPECT_EQ(r.id(), "handler:orders::validate");
}

// ── index.toml + version-range path selection (§4.2) ────────────────────

TEST(MigrationIndex, ParsesTwoStepIndex) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx) << err;
    EXPECT_TRUE(exists);
    ASSERT_EQ(idx->entries.size(), 2u);
    EXPECT_EQ(idx->entries[0].toVersion, "0.3.0");
    EXPECT_EQ(idx->entries[1].toVersion, "1.0.0");
}

TEST(MigrationIndex, AbsentFileIsNotAnError) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("does-not-exist.toml"), exists,
                                    err);
    ASSERT_TRUE(idx);
    EXPECT_FALSE(exists);
    EXPECT_TRUE(idx->entries.empty());
}

TEST(MigrationIndex, SelectsSingleStepPath) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx);

    auto path = idx->selectPath("0.2.1", "0.3.0", err);
    ASSERT_TRUE(path) << err;
    ASSERT_EQ(path->size(), 1u);
    EXPECT_EQ((*path)[0].toVersion, "0.3.0");
}

TEST(MigrationIndex, SelectsMultiStepChain) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx);

    auto path = idx->selectPath("0.2.0", "1.0.0", err);
    ASSERT_TRUE(path) << err;
    ASSERT_EQ(path->size(), 2u);
    EXPECT_EQ((*path)[0].toVersion, "0.3.0");
    EXPECT_EQ((*path)[1].toVersion, "1.0.0");
}

TEST(MigrationIndex, ReportsCoverageGap) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx);

    // 2.0.0 is beyond the last `to` — no step covers the tail.
    auto path = idx->selectPath("0.2.0", "2.0.0", err);
    EXPECT_FALSE(path);
    EXPECT_NE(err.find("coverage gap"), std::string::npos);
}

TEST(MigrationIndex, AlreadyAtTargetIsEmptyPath) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx);

    auto path = idx->selectPath("1.0.0", "1.0.0", err);
    ASSERT_TRUE(path);
    EXPECT_TRUE(path->empty());
}

TEST(MigrationIndex, RejectsDowngrade) {
    std::string err;
    bool exists = false;
    auto idx = MigrationIndex::load(fixture("index.toml"), exists, err);
    ASSERT_TRUE(idx);

    auto path = idx->selectPath("1.0.0", "0.3.0", err);
    EXPECT_FALSE(path);
    EXPECT_NE(err.find("downgrade"), std::string::npos);
}

// ── Duplicate rule id rejection (tpm-extensibility-fragilities §3) ──────

#include <cstdio>
#include <filesystem>

namespace {
std::string writeTempFile(const std::string& contents) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() /
        ("tpm-migration-dup-" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         std::to_string(reinterpret_cast<std::uintptr_t>(&contents)) +
         ".toml");
    std::FILE* f = std::fopen(path.string().c_str(), "w");
    if (!f) std::abort();
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    return path.string();
}
} // namespace

TEST(MigrationRule, RejectsDuplicateKindTargetWithinFile) {
    // Two rules sharing (kind=handler, target=orders::validate) would
    // produce two indistinguishable entries in the migration report and
    // the engine would apply both — see tpm-extensibility-fragilities §3.
    const std::string toml =
        "[[rule]]\n"
        "kind = \"handler\"\n"
        "target = \"orders::validate\"\n"
        "note = \"first rule\"\n"
        "\n"
        "[[rule]]\n"
        "kind = \"handler\"\n"
        "target = \"orders::validate\"\n"
        "note = \"second rule with same id\"\n";
    std::string path = writeTempFile(toml);
    std::string err;
    auto set = MigrationRuleSet::load(path, err);
    EXPECT_FALSE(set);
    EXPECT_NE(err.find("duplicate rule id"), std::string::npos)
        << "expected 'duplicate rule id' in error, got: " << err;
    EXPECT_NE(err.find("handler:orders::validate"), std::string::npos)
        << "error should name the colliding id, got: " << err;
    std::filesystem::remove(path);
}

TEST(MigrationRule, AcceptsDifferentTargetsWithSameKind) {
    const std::string toml =
        "[[rule]]\n"
        "kind = \"handler\"\n"
        "target = \"orders::validate\"\n"
        "\n"
        "[[rule]]\n"
        "kind = \"handler\"\n"
        "target = \"orders::reject\"\n";
    std::string path = writeTempFile(toml);
    std::string err;
    auto set = MigrationRuleSet::load(path, err);
    EXPECT_TRUE(set) << err;
    EXPECT_EQ(set->rules.size(), 2u);
    std::filesystem::remove(path);
}

// ── Manifest schema version (tpm-extensibility-fragilities §2) ──────────

#include "tpm/Manifest.h"

TEST(Manifest, DefaultsSchemaVersionTo1WhenAbsent) {
    const std::string toml =
        "[package]\n"
        "name = \"acme/orders\"\n"
        "version = \"0.1.0\"\n"
        "kind = \"declaration\"\n"
        "license = \"MIT\"\n"
        "core_compat = \">=4.0.0, <5.0.0\"\n";
    std::string path = writeTempFile(toml);
    std::string err;
    auto m = Manifest::load(path, err);
    ASSERT_TRUE(m) << err;
    EXPECT_EQ(m->manifestSchemaVersion, "1")
        << "missing field must default to '1' for back-compat";
    std::filesystem::remove(path);
}

TEST(Manifest, ReadsExplicitSchemaVersion) {
    const std::string toml =
        "[package]\n"
        "name = \"acme/orders\"\n"
        "version = \"0.1.0\"\n"
        "manifest_version = \"1.2.0\"\n"
        "kind = \"declaration\"\n"
        "license = \"MIT\"\n"
        "core_compat = \">=4.0.0, <5.0.0\"\n";
    std::string path = writeTempFile(toml);
    std::string err;
    auto m = Manifest::load(path, err);
    ASSERT_TRUE(m) << err;
    EXPECT_EQ(m->manifestSchemaVersion, "1.2.0");
    std::filesystem::remove(path);
}

TEST(Manifest, RejectsFutureMajorSchemaVersion) {
    const std::string toml =
        "[package]\n"
        "name = \"acme/orders\"\n"
        "version = \"0.1.0\"\n"
        "manifest_version = \"99.0.0\"\n"
        "kind = \"declaration\"\n"
        "license = \"MIT\"\n"
        "core_compat = \">=4.0.0, <5.0.0\"\n";
    std::string path = writeTempFile(toml);
    std::string err;
    auto m = Manifest::load(path, err);
    EXPECT_FALSE(m);
    EXPECT_NE(err.find("manifest_version"), std::string::npos);
    EXPECT_NE(err.find("newer tpm"), std::string::npos);
    std::filesystem::remove(path);
}

TEST(Manifest, ToTomlEmitsSchemaVersion) {
    Manifest m;
    m.name = "acme/orders";
    m.version = "0.1.0";
    m.license = "MIT";
    m.coreCompat = ">=4.0.0, <5.0.0";
    m.kind = PackageKind::Declaration;
    std::string out = m.toToml();
    EXPECT_NE(out.find("manifest_version = \"1\""), std::string::npos)
        << "toToml must emit the schema-version field; got:\n" << out;
}
