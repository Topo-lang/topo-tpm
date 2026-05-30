#ifndef TPM_GIT_REGISTRY_H
#define TPM_GIT_REGISTRY_H

#include <optional>
#include <string>
#include <vector>

#include "tpm/SemVer.h"

namespace tpm {

/// Result of resolving one dependency against a git repository.
struct ResolvedGitPackage {
    std::string tag;         // the selected git tag
    SemVer version;          // SemVer parsed from the tag
    std::string revision;    // commit SHA the tag points to
};

/// git-based registry driver. A package is a git
/// repository and a published version is a git tag. All git invocations go
/// through `topo::platform::runProcessCapture` — no shell, no implicit network
/// outside these explicit calls.
class GitRegistry {
public:
    /// True when the `git` executable is resolvable on PATH.
    static bool gitAvailable();

    /// List the SemVer-shaped tags of a remote repository (via `git ls-remote
    /// --tags`). Non-SemVer tags are skipped. Returns nullopt on git failure
    /// and fills `error`.
    std::optional<std::vector<std::pair<std::string, SemVer>>>
    listRemoteTags(const std::string& repoUrl, std::string& error) const;

    /// Resolve a dependency: pick the highest tag satisfying `versionReq`.
    /// Returns nullopt on no-match or git failure (`error` is filled).
    std::optional<ResolvedGitPackage> resolve(const std::string& repoUrl,
                                              const std::string& versionReq,
                                              std::string& error) const;

    /// Clone `repoUrl` at `tag` into `destDir` (a fresh directory). Uses a
    /// shallow clone. Returns true on success; on failure fills `error`.
    bool fetchInto(const std::string& repoUrl, const std::string& tag,
                   const std::string& destDir, std::string& error) const;
};

} // namespace tpm

#endif // TPM_GIT_REGISTRY_H
