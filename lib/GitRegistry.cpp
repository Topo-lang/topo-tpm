#include "tpm/GitRegistry.h"

#include "topo/Platform/Process.h"

#include <algorithm>
#include <sstream>

namespace tpm {

bool GitRegistry::gitAvailable() {
    // `git` is expected on PATH; tpm bundles no git binary. A zero exit from
    // `git --version` confirms it is resolvable.
    auto r = topo::platform::runProcessCapture("git", {"--version"});
    return r.exitCode == 0;
}

std::optional<std::vector<std::pair<std::string, SemVer>>>
GitRegistry::listRemoteTags(const std::string& repoUrl,
                            std::string& error) const {
    // `git ls-remote --tags <url>` reads tags without a full clone.
    auto r = topo::platform::runProcessCapture(
        "git", {"ls-remote", "--tags", repoUrl});
    if (r.exitCode != 0) {
        error = "git ls-remote failed for '" + repoUrl + "': " + r.stderrOutput;
        return std::nullopt;
    }

    std::vector<std::pair<std::string, SemVer>> tags;
    std::istringstream iss(r.stdoutOutput);
    std::string line;
    while (std::getline(iss, line)) {
        // Each line: "<sha>\trefs/tags/<tag>" (peeled tags end in "^{}").
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string ref = line.substr(tab + 1);
        const std::string prefix = "refs/tags/";
        if (ref.rfind(prefix, 0) != 0) continue;
        std::string tag = ref.substr(prefix.size());
        if (tag.size() > 3 && tag.substr(tag.size() - 3) == "^{}")
            tag = tag.substr(0, tag.size() - 3);
        auto v = SemVer::parse(tag);
        if (!v) continue; // skip non-SemVer tags
        // De-dup (peeled + unpeeled refs map to the same tag).
        bool seen = false;
        for (const auto& t : tags)
            if (t.first == tag) { seen = true; break; }
        if (!seen) tags.emplace_back(tag, *v);
    }
    return tags;
}

std::optional<ResolvedGitPackage>
GitRegistry::resolve(const std::string& repoUrl, const std::string& versionReq,
                     std::string& error) const {
    auto req = VersionReq::parse(versionReq);
    if (!req) {
        error = "invalid version requirement '" + versionReq + "'";
        return std::nullopt;
    }

    auto tags = listRemoteTags(repoUrl, error);
    if (!tags) return std::nullopt;

    // Highest SemVer tag satisfying the requirement.
    const std::pair<std::string, SemVer>* best = nullptr;
    for (const auto& t : *tags) {
        if (!req->matches(t.second)) continue;
        if (!best || best->second < t.second) best = &t;
    }
    if (!best) {
        error = "no tag of '" + repoUrl + "' satisfies '" + versionReq + "'";
        return std::nullopt;
    }

    // Resolve the tag to its commit SHA.
    auto r = topo::platform::runProcessCapture(
        "git", {"ls-remote", repoUrl, "refs/tags/" + best->first});
    std::string revision;
    if (r.exitCode == 0) {
        // Prefer the peeled ("^{}") line — that is the commit the annotated
        // tag points at.
        std::istringstream iss(r.stdoutOutput);
        std::string line, plain;
        while (std::getline(iss, line)) {
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string sha = line.substr(0, tab);
            std::string ref = line.substr(tab + 1);
            if (ref.size() > 3 && ref.substr(ref.size() - 3) == "^{}")
                revision = sha;
            else if (plain.empty())
                plain = sha;
        }
        if (revision.empty()) revision = plain;
    }

    ResolvedGitPackage out;
    out.tag = best->first;
    out.version = best->second;
    out.revision = revision;
    return out;
}

bool GitRegistry::fetchInto(const std::string& repoUrl, const std::string& tag,
                            const std::string& destDir,
                            std::string& error) const {
    // Shallow clone of exactly one tag.
    auto r = topo::platform::runProcessCapture(
        "git", {"clone", "--depth", "1", "--branch", tag, repoUrl, destDir});
    if (r.exitCode != 0) {
        error = "git clone failed for '" + repoUrl + "@" + tag +
                "': " + r.stderrOutput;
        return false;
    }
    return true;
}

} // namespace tpm
