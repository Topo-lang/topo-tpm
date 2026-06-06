#ifndef TPM_LOCK_H
#define TPM_LOCK_H

#include <optional>
#include <string>
#include <vector>

namespace tpm {

/// One pinned, fully-resolved package.
struct LockedPackage {
    std::string name;
    std::string version;       // exact resolved version (not a range)
    std::string source;        // git URL + tag, or central-registry coordinate
    std::string revision;      // exact git commit SHA the tag pointed to
    std::string contentHash;   // SHA-256 over the package's normalized file set
};

/// `tpm.lock` — generated, not hand-edited. Pins the dependency graph.
struct Lock {
    std::vector<LockedPackage> packages;

    /// Load tpm.lock. On a parse error, returns nullopt and fills `error`.
    /// An absent file is reported via `exists` (not an error). Every loaded
    /// entry is run through `validate()` — a tpm.lock is generated, never
    /// hand-edited, so a `name`/`version` carrying a path separator or `..`
    /// is a tampered file and is rejected at load rather than allowed to
    /// compose a cache path that escapes `.topo-pkgs/`.
    static std::optional<Lock> load(const std::string& path, bool& exists,
                                    std::string& error);

    /// Reject `name`/`version` fields that could escape the package cache
    /// when joined into `.topo-pkgs/<name>/<version>` (path separators or
    /// `..`). Mirrors `Manifest::validate()`'s path-traversal defence for
    /// the lock surface, which feeds the same `cache.packageDir(...)` ->
    /// `fs::remove_all` / `create_directories` path. Returns the list of
    /// problems (empty == clean).
    std::vector<std::string> validate() const;

    /// Serialize to the canonical tpm.lock TOML form.
    std::string toToml() const;

    /// Look up a pinned package by name.
    const LockedPackage* find(const std::string& name) const;
};

} // namespace tpm

#endif // TPM_LOCK_H
