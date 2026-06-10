#ifndef TPM_MANIFEST_H
#define TPM_MANIFEST_H

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tpm {

/// Package kinds. Exactly one per package.
///
/// `Count` is a sentinel for size-of-enum compile-time checks (see
/// `tpm/lib/Commands.cpp` — `kDirRules` width is `static_assert`ed
/// against it). When adding a new kind, insert it before `Count` and
/// extend every `kDirRules` row's `requiredForKind` array.
enum class PackageKind { Declaration, Layout, EventProtocol, StdlibType, Kernel, Count };

std::optional<PackageKind> parseKind(const std::string& text);
const char* kindToString(PackageKind kind);

/// Whether `kind` carries `.topo` declarations whose leaves can be adapted —
/// the kinds on which `[[adapters]]` and an `adapters/` directory are
/// accepted (package-format spec §1.4). `layout` / `event-protocol` carry no
/// declarations and reject `[[adapters]]`.
bool isDeclarationBearingKind(PackageKind kind);

/// Validate a per-dependency `registry` URL before it is handed to `git`
/// as a positional argument. Blocks the git argument-injection class
/// (CVE-2017-1000117): values that begin with `-` (interpreted by git as
/// options, e.g. `--upload-pack=…`) and arbitrary-command transports
/// (`ext::`, `fd::`). Only an allow-listed scheme set is accepted
/// (`https://`, `http://`, `ssh://`, `git://`, `file://`, or scp-like
/// `user@host:path`); an optional `git+` prefix is stripped first.
/// Returns an empty string when the URL is acceptable, else a diagnostic.
std::string validateRegistryUrl(const std::string& url);

/// Schema version of the `tpm.toml` manifest format itself (NOT the
/// user's package version — that's `Manifest::version`). Default `"1"`
/// when the manifest omits `[package].manifest_version` (back-compat).
/// Bump policy: SemVer; major bump is reserved for incompatible field
/// reshapes that older tpm versions cannot read.
constexpr const char* kCurrentManifestSchemaVersion = "1";

/// One [dependencies] entry: a SemVer requirement plus an optional explicit
/// registry source override.
struct Dependency {
    std::string name;            // "<namespace>/<name>"
    std::string versionReq;      // SemVer requirement string
    std::string registry;        // optional git URL / central-registry name override
    std::string contentHash;     // optional publisher-pinned content hash; when
                                 // set, the FIRST install verifies against it
                                 // instead of trust-on-first-use
};

/// One [bindings] host entry — advisory metadata only.
struct Binding {
    std::string host;            // cpp / rust / java / python / typescript
    std::string manager;         // vcpkg / cargo / pip / maven
    std::string packageId;       // package id in the host manager
    std::string version;         // optional
};

/// One [[adapters]] entry — a library/API scenario-adaptation pair this
/// package provides. The varying axis is the library/API, not the language:
/// the source and target language are the same (cpp→cpp is the norm). See
/// package-format spec §1.2. The leaf-adapter manifests that realize the
/// pair live under the package's `adapters/` directory; the condition model
/// that selects *when* a pair applies is a separate design (not here).
struct AdapterPair {
    std::string fromLibrary;             // source library adapted away from
    std::string toLibrary;               // target library adapted to
    std::vector<std::string> languages;  // host language(s); ≥1, same-language ok
};

/// Parsed `tpm.toml`.
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
    /// [[adapters]] — the library/API scenario-adaptation pairs this package
    /// provides (package-format §1.2). When non-empty the package must carry
    /// a non-empty `adapters/` directory and `kind` must be
    /// declaration-bearing; `validate()` / `tpm verify` enforce both.
    std::vector<AdapterPair> adapters;

    /// Load and parse a tpm.toml file. On failure, returns nullopt and fills
    /// `error` with a human-readable reason.
    static std::optional<Manifest> load(const std::string& path, std::string& error);

    /// Validate against the manifest schema (required fields, kind enum,
    /// core_compat is a parseable range, name shape). Returns the list of
    /// problems; empty == valid.
    std::vector<std::string> validate() const;

    /// Serialize back to TOML text (used by `tpm init` / `tpm add` / `tpm remove`).
    std::string toToml() const;
};

} // namespace tpm

#endif // TPM_MANIFEST_H
