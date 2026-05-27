#ifndef TPM_LOCK_H
#define TPM_LOCK_H

#include <optional>
#include <string>
#include <vector>

namespace tpm {

/// One pinned, fully-resolved package (package-format spec §1.3).
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
    /// An absent file is reported via `exists` (not an error).
    static std::optional<Lock> load(const std::string& path, bool& exists,
                                    std::string& error);

    /// Serialize to the canonical tpm.lock TOML form.
    std::string toToml() const;

    /// Look up a pinned package by name.
    const LockedPackage* find(const std::string& name) const;
};

} // namespace tpm

#endif // TPM_LOCK_H
