#include "tpm/PackageHash.h"

#include "tpm/Sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace tpm {

std::string computePackageContentHash(const std::string& packageDir) {
    fs::path root(packageDir);
    if (!fs::exists(root) || !fs::is_directory(root)) return "";

    std::vector<std::pair<std::string, fs::path>> files; // relpath -> abs
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        // Exclude VCS metadata and the generated lock from the content hash.
        const std::string fname = p.filename().string();
        // Audit issue tpm-path-traversal-via-untrusted-manifest-fields:
        // ``is_directory`` / ``is_regular_file`` follow symlinks by
        // default. A symlink in the package directory pointing to
        // ``/etc/passwd`` would otherwise have its target's content
        // folded into the content hash. Skip every symlink entry via
        // ``symlink_status`` (which does NOT follow) — the package
        // owner can ship symlinks as part of the source tree but they
        // do not contribute target-file content to the hash.
        if (it->symlink_status(ec).type() == fs::file_type::symlink)
            continue;
        if (it->is_directory()) {
            if (fname == ".git") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (fname == "tpm.lock") continue;

        fs::path rel = fs::relative(p, root, ec);
        if (ec) continue;
        std::string relStr = rel.generic_string(); // '/' separators
        files.emplace_back(relStr, p);
    }

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string stream;
    for (const auto& [rel, abs] : files) {
        stream.append(rel);
        stream.push_back('\0');
        std::ifstream in(abs, std::ios::binary);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            stream.append(content);
        }
        stream.push_back('\0');
    }

    return sha256Hex(stream);
}

} // namespace tpm
