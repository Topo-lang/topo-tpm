#include "tpm/Manifest.h"

#include "tpm/SemVer.h"

#include "topo/Platform/Sanitize.h"

// toml++ in header-only, no-exception mode (consistent with topo-build).
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <cctype>
#include <sstream>

namespace tpm {

std::optional<PackageKind> parseKind(const std::string& text) {
    if (text == "declaration") return PackageKind::Declaration;
    if (text == "layout") return PackageKind::Layout;
    if (text == "event-protocol") return PackageKind::EventProtocol;
    if (text == "stdlib-type") return PackageKind::StdlibType;
    if (text == "kernel") return PackageKind::Kernel;
    return std::nullopt;
}

const char* kindToString(PackageKind kind) {
    switch (kind) {
    case PackageKind::Declaration: return "declaration";
    case PackageKind::Layout: return "layout";
    case PackageKind::EventProtocol: return "event-protocol";
    case PackageKind::StdlibType: return "stdlib-type";
    case PackageKind::Kernel: return "kernel";
    case PackageKind::Count: break;  // sentinel; never a real value
    }
    return "declaration";
}

bool isDeclarationBearingKind(PackageKind kind) {
    return kind == PackageKind::Declaration ||
           kind == PackageKind::StdlibType ||
           kind == PackageKind::Kernel;
}

namespace {

/// A host-language name accepted in `[[adapters]].languages` (matches the
/// HostLanguage set in topo-core).
bool isHostLanguage(const std::string& s) {
    return s == "cpp" || s == "rust" || s == "java" || s == "python" ||
           s == "typescript";
}

bool isKebabSegment(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!(std::islower(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)) || c == '-'))
            return false;
    }
    return s.front() != '-' && s.back() != '-';
}

} // namespace

std::optional<Manifest> Manifest::load(const std::string& path, std::string& error) {
    toml::parse_result result = toml::parse_file(path);
    if (!result) {
        std::ostringstream os;
        os << "failed to parse " << path << ": " << result.error().description();
        error = os.str();
        return std::nullopt;
    }
    const toml::table& tbl = result.table();

    Manifest m;

    const toml::table* pkg = tbl["package"].as_table();
    if (!pkg) {
        error = "tpm.toml: missing required [package] section";
        return std::nullopt;
    }

    auto str = [](const toml::table& t, const char* key) -> std::string {
        if (auto v = t[key].value<std::string>()) return *v;
        return "";
    };

    m.name = str(*pkg, "name");
    m.version = str(*pkg, "version");
    m.license = str(*pkg, "license");
    m.coreCompat = str(*pkg, "core_compat");
    m.description = str(*pkg, "description");
    m.repository = str(*pkg, "repository");

    // Manifest schema version. Defaults to "1" when the field is
    // absent (manifests written before the field existed). When
    // present, must be a SemVer; major mismatch with
    // kCurrentManifestSchemaVersion is rejected as future-format.
    if (auto mv = (*pkg)["manifest_version"].value<std::string>()) {
        m.manifestSchemaVersion = *mv;
        auto parsed = SemVer::parse(*mv);
        if (!parsed) {
            error = "tpm.toml: [package].manifest_version '" + *mv +
                    "' is not valid SemVer";
            return std::nullopt;
        }
        auto current = SemVer::parse(kCurrentManifestSchemaVersion);
        if (current && parsed->major > current->major) {
            error = "tpm.toml: [package].manifest_version " + *mv +
                    " requires a newer tpm (this tpm supports up to " +
                    kCurrentManifestSchemaVersion + ")";
            return std::nullopt;
        }
    } else if (pkg->contains("manifest_version")) {
        error = "tpm.toml: [package].manifest_version must be a string";
        return std::nullopt;
    }

    if (auto kindStr = (*pkg)["kind"].value<std::string>()) {
        auto k = parseKind(*kindStr);
        if (!k) {
            error = "tpm.toml: [package].kind has invalid value '" + *kindStr +
                    "' (expected declaration / layout / event-protocol / "
                    "stdlib-type / kernel)";
            return std::nullopt;
        }
        m.kind = *k;
    } else if (pkg->contains("kind")) {
        error = "tpm.toml: [package].kind must be a string";
        return std::nullopt;
    }

    if (const toml::array* arr = (*pkg)["authors"].as_array()) {
        for (const auto& el : *arr)
            if (auto s = el.value<std::string>()) m.authors.push_back(*s);
    }

    if (const toml::table* deps = tbl["dependencies"].as_table()) {
        for (const auto& [key, node] : *deps) {
            Dependency dep;
            dep.name = std::string(key.str());
            if (auto s = node.value<std::string>()) {
                dep.versionReq = *s;
            } else if (const toml::table* dt = node.as_table()) {
                dep.versionReq = str(*dt, "version");
                dep.registry = str(*dt, "registry");
            } else {
                error = "tpm.toml: dependency '" + dep.name +
                        "' must be a version string or a table";
                return std::nullopt;
            }
            m.dependencies.push_back(std::move(dep));
        }
    }

    if (const toml::table* binds = tbl["bindings"].as_table()) {
        for (const auto& [key, node] : *binds) {
            const toml::table* bt = node.as_table();
            if (!bt) {
                error = "tpm.toml: [bindings]." + std::string(key.str()) +
                        " must be a table";
                return std::nullopt;
            }
            Binding b;
            b.host = std::string(key.str());
            b.version = str(*bt, "version");
            for (const char* mgr : {"vcpkg", "cargo", "pip", "maven"}) {
                if (auto s = (*bt)[mgr].value<std::string>()) {
                    b.manager = mgr;
                    b.packageId = *s;
                    break;
                }
            }
            m.bindings.push_back(std::move(b));
        }
    }

    // [[adapters]] — array of tables, each a library/API adaptation pair.
    if (const toml::array* arr = tbl["adapters"].as_array()) {
        for (const auto& el : *arr) {
            const toml::table* at = el.as_table();
            if (!at) {
                error = "tpm.toml: [[adapters]] entries must be tables";
                return std::nullopt;
            }
            AdapterPair ap;
            ap.fromLibrary = str(*at, "from_library");
            ap.toLibrary = str(*at, "to_library");
            if (const toml::array* langs = (*at)["languages"].as_array()) {
                for (const auto& le : *langs)
                    if (auto s = le.value<std::string>())
                        ap.languages.push_back(*s);
            } else if (at->contains("languages")) {
                error = "tpm.toml: [[adapters]].languages must be an array of "
                        "strings";
                return std::nullopt;
            }
            m.adapters.push_back(std::move(ap));
        }
    }

    return m;
}

std::string validateRegistryUrl(const std::string& raw) {
    if (raw.empty()) return "registry URL is empty";

    // Strip an optional `git+` scheme prefix (mirrors resolveDependencies);
    // validate what `git` will actually receive.
    std::string url = raw;
    if (url.rfind("git+", 0) == 0) url = url.substr(4);

    if (url.empty() || url[0] == '-')
        return "registry URL '" + raw +
               "' starts with '-'; option-injection payload rejected";

    auto startsWithCI = [&url](const char* p) {
        size_t n = 0;
        while (p[n]) ++n;
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) !=
                std::tolower(static_cast<unsigned char>(p[i])))
                return false;
        return true;
    };

    // Arbitrary-command git transports — always rejected.
    for (const char* bad : {"ext::", "fd::"})
        if (startsWithCI(bad))
            return "registry URL '" + raw + "' uses the disallowed '" +
                   std::string(bad) + "' git transport";

    // Allow-listed URL schemes.
    for (const char* scheme : {"https://", "http://", "ssh://", "git://",
                               "file://"})
        if (startsWithCI(scheme)) return "";

    // scp-like syntax: user@host:path — require the '@' (matching the
    // documented `git@…` form) so a bare `scheme:junk` cannot pass as a
    // host:path. The ':' must precede any '/'.
    size_t at = url.find('@');
    size_t colon = url.find(':');
    size_t slash = url.find('/');
    if (at != std::string::npos && colon != std::string::npos && at > 0 &&
        at < colon && (slash == std::string::npos || colon < slash))
        return "";

    return "registry URL '" + raw +
           "' has no allow-listed scheme (https://, ssh://, git://, file://, "
           "or user@host:path)";
}

std::vector<std::string> Manifest::validate() const {
    std::vector<std::string> problems;

    if (name.empty()) {
        problems.push_back("[package].name is required");
    } else {
        size_t slash = name.find('/');
        if (slash == std::string::npos) {
            problems.push_back(
                "[package].name must have the form '<namespace>/<name>'");
        } else {
            std::string ns = name.substr(0, slash);
            std::string nm = name.substr(slash + 1);
            if (!isKebabSegment(ns) || !isKebabSegment(nm))
                problems.push_back("[package].name namespace and name must be "
                                   "kebab-case (lowercase letters, digits, "
                                   "hyphens)");
        }
    }

    if (version.empty()) {
        problems.push_back("[package].version is required");
    } else if (!SemVer::parse(version)) {
        problems.push_back("[package].version '" + version +
                           "' is not valid SemVer 2.0.0");
    } else {
        // Audit: untrusted manifest fields enabling path traversal.
        // SemVer prerelease strings (anything after ``-``) accept
        // arbitrary text by spec, including ``../``. We later join the
        // version into ``.topo-pkgs/<name>/<version>`` to compute a
        // cache destination, so a malicious manifest with
        // ``version = "1.0.0-../../etc/passwd"`` would otherwise write
        // outside the cache root. Reject any path-segment metacharacter
        // explicitly. ``+`` (SemVer build metadata) is also rejected so
        // even a benign user does not accidentally produce a directory
        // that confuses lookups.
        for (char c : version) {
            if (c == '/' || c == '\\') {
                problems.push_back(
                    "[package].version '" + version +
                    "' contains a path separator; path-traversal payload "
                    "rejected");
                break;
            }
        }
        if (version.find("..") != std::string::npos) {
            problems.push_back(
                "[package].version '" + version +
                "' contains '..'; path-traversal payload rejected");
        }
    }

    if (license.empty())
        problems.push_back("[package].license is required (SPDX expression)");

    if (coreCompat.empty()) {
        problems.push_back("[package].core_compat is required (a SemVer range)");
    } else if (!VersionReq::parse(coreCompat)) {
        problems.push_back("[package].core_compat '" + coreCompat +
                           "' is not a parseable SemVer range");
    }

    for (const auto& dep : dependencies) {
        if (dep.versionReq.empty()) {
            problems.push_back("dependency '" + dep.name +
                               "' has no version requirement");
        } else if (!VersionReq::parse(dep.versionReq)) {
            problems.push_back("dependency '" + dep.name +
                               "' has an invalid version requirement '" +
                               dep.versionReq + "'");
        }
        if (dep.name.find('/') == std::string::npos)
            problems.push_back("dependency '" + dep.name +
                               "' must have the form '<namespace>/<name>'");
        if (!dep.registry.empty()) {
            std::string regErr = validateRegistryUrl(dep.registry);
            if (!regErr.empty())
                problems.push_back("dependency '" + dep.name + "' " + regErr);
        }
    }

    // [[adapters]] — each pair needs a non-empty source/target library and at
    // least one valid host language; the section is accepted only on a
    // declaration-bearing kind (package-format §1.2/§1.4).
    for (const auto& ap : adapters) {
        std::string pairName =
            "'" + ap.fromLibrary + "' → '" + ap.toLibrary + "'";
        if (ap.fromLibrary.empty())
            problems.push_back("[[adapters]] entry has an empty 'from_library'");
        if (ap.toLibrary.empty())
            problems.push_back("[[adapters]] entry has an empty 'to_library'");
        if (ap.languages.empty())
            problems.push_back("[[adapters]] entry " + pairName +
                               " has no 'languages' (need ≥1)");
        for (const auto& l : ap.languages)
            if (!isHostLanguage(l))
                problems.push_back("[[adapters]] entry " + pairName +
                                   " has invalid language '" + l +
                                   "' (expected cpp / rust / java / python / "
                                   "typescript)");
    }
    if (!adapters.empty() && !isDeclarationBearingKind(kind))
        problems.push_back(
            std::string("[[adapters]] is only valid on a declaration-bearing "
                        "kind (declaration / stdlib-type / kernel), not '") +
            kindToString(kind) + "'");

    return problems;
}

std::string Manifest::toToml() const {
    std::ostringstream os;
    os << "[package]\n";
    os << "name = \"" << name << "\"\n";
    os << "version = \"" << version << "\"\n";
    os << "manifest_version = \"" << manifestSchemaVersion << "\"\n";
    os << "kind = \"" << kindToString(kind) << "\"\n";
    os << "license = \"" << license << "\"\n";
    os << "core_compat = \"" << coreCompat << "\"\n";
    if (!description.empty())
        os << "description = \"" << description << "\"\n";
    if (!authors.empty()) {
        os << "authors = [";
        for (size_t i = 0; i < authors.size(); ++i) {
            if (i) os << ", ";
            os << '"' << authors[i] << '"';
        }
        os << "]\n";
    }
    if (!repository.empty())
        os << "repository = \"" << repository << "\"\n";

    if (!dependencies.empty()) {
        os << "\n[dependencies]\n";
        for (const auto& dep : dependencies) {
            os << '"' << dep.name << "\" = ";
            if (dep.registry.empty()) {
                os << '"' << dep.versionReq << "\"\n";
            } else {
                os << "{ version = \"" << dep.versionReq
                   << "\", registry = \"" << dep.registry << "\" }\n";
            }
        }
    }

    if (!bindings.empty()) {
        os << "\n[bindings]\n";
        for (const auto& b : bindings) {
            os << b.host << " = { " << b.manager << " = \"" << b.packageId
               << '"';
            if (!b.version.empty())
                os << ", version = \"" << b.version << '"';
            os << " }\n";
        }
    }

    for (const auto& ap : adapters) {
        os << "\n[[adapters]]\n";
        os << "from_library = \"" << ap.fromLibrary << "\"\n";
        os << "to_library = \"" << ap.toLibrary << "\"\n";
        os << "languages = [";
        for (size_t i = 0; i < ap.languages.size(); ++i) {
            if (i) os << ", ";
            os << '"' << ap.languages[i] << '"';
        }
        os << "]\n";
    }

    return os.str();
}

} // namespace tpm
