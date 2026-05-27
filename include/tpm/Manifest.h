#ifndef TPM_MANIFEST_H
#define TPM_MANIFEST_H

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tpm {

/// Package kinds (package-format spec §1.1). Exactly one per package.
///
/// `Count` is a sentinel for size-of-enum compile-time checks (see
/// `tpm/lib/Commands.cpp` — `kDirRules` width is `static_assert`ed
/// against it). When adding a new kind, insert it before `Count` and
/// extend every `kDirRules` row's `requiredForKind` array.
enum class PackageKind { Declaration, Layout, EventProtocol, StdlibType, Kernel, Count };

std::optional<PackageKind> parseKind(const std::string& text);
const char* kindToString(PackageKind kind);

/// Schema version of the `tpm.toml` manifest format itself (NOT the
/// user's package version — that's `Manifest::version`). Default `"1"`
/// when the manifest omits `[package].manifest_version` (back-compat).
/// Bump policy: SemVer; major bump is reserved for incompatible field
/// reshapes that older tpm versions cannot read.
constexpr const char* kCurrentManifestSchemaVersion = "1";

/// One [dependencies] entry: a SemVer requirement plus an optional explicit
/// registry source override (package-format spec §1.2).
struct Dependency {
    std::string name;            // "<namespace>/<name>"
    std::string versionReq;      // SemVer requirement string
    std::string registry;        // optional git URL / central-registry name override
};

/// One [bindings] host entry — advisory metadata only (spec §1.2).
struct Binding {
    std::string host;            // cpp / rust / java / python / typescript
    std::string manager;         // vcpkg / cargo / pip / maven
    std::string packageId;       // package id in the host manager
    std::string version;         // optional
};

/// Parsed `tpm.toml`. Field set follows package-format spec §1.2.
struct Manifest {
    // [package]
    std::string name;
    std::string version;
    PackageKind kind = PackageKind::Declaration;
    std::string license;
    std::string coreCompat;      // SemVer range
    std::string description;     // optional
    std::vector<std::string> authors;
    std::string repository;      // optional
    /// Schema version of the manifest format. Reads
    /// `[package].manifest_version`; defaults to "1" when the field is
    /// absent (back-compat with manifests written before the field
    /// existed). See `kCurrentManifestSchemaVersion`.
    std::string manifestSchemaVersion = "1";

    std::vector<Dependency> dependencies;
    std::vector<Binding> bindings;

    /// Load and parse a tpm.toml file. On failure, returns nullopt and fills
    /// `error` with a human-readable reason.
    static std::optional<Manifest> load(const std::string& path, std::string& error);

    /// Validate against the spec schema (required fields, kind enum,
    /// core_compat is a parseable range, name shape). Returns the list of
    /// problems; empty == valid.
    std::vector<std::string> validate() const;

    /// Serialize back to TOML text (used by `tpm init` / `tpm add` / `tpm remove`).
    std::string toToml() const;
};

} // namespace tpm

#endif // TPM_MANIFEST_H
