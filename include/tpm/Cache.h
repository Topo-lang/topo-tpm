#ifndef TPM_CACHE_H
#define TPM_CACHE_H

#include <string>

namespace tpm {

/// Project-local package cache. The MVP picks the project-local
/// `.topo-pkgs/` layout; a global cache is left for later. Packages land
/// at `.topo-pkgs/<namespace>/<name>/<version>/`.
class Cache {
public:
    /// Construct a cache rooted at `<projectRoot>/.topo-pkgs`.
    explicit Cache(const std::string& projectRoot);

    /// Absolute path of the cache root.
    const std::string& root() const { return root_; }

    /// Absolute install directory for a (name, version) pair. Does not create it.
    std::string packageDir(const std::string& name,
                           const std::string& version) const;

    /// True when a package is already installed at that directory.
    bool isInstalled(const std::string& name,
                     const std::string& version) const;

private:
    std::string root_;
};

} // namespace tpm

#endif // TPM_CACHE_H
