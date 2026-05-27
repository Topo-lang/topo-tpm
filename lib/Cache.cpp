#include "tpm/Cache.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace tpm {

Cache::Cache(const std::string& projectRoot) {
    root_ = (fs::path(projectRoot) / ".topo-pkgs").string();
}

std::string Cache::packageDir(const std::string& name,
                              const std::string& version) const {
    // name is "<namespace>/<name>"; map directly onto nested directories.
    return (fs::path(root_) / name / version).string();
}

bool Cache::isInstalled(const std::string& name,
                        const std::string& version) const {
    fs::path dir = packageDir(name, version);
    return fs::exists(dir) && fs::exists(dir / "tpm.toml");
}

} // namespace tpm
