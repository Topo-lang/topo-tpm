#include "tpm/Commands.h"

#include "tpm/Cache.h"
#include "tpm/GitRegistry.h"
#include "tpm/Lock.h"
#include "tpm/Manifest.h"
#include "tpm/MigrationEngine.h"
#include "tpm/MigrationRule.h"
#include "tpm/PackageHash.h"
#include "tpm/SemVer.h"

#include "topo/Platform/FileLock.h"
#include "topo/Platform/Sanitize.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace tpm {
namespace {

// ── kind → directory layout ─────────────────────────────────────────────

// PackageKind has 5 real values plus `Count` sentinel; the array width
// must match the real value count exactly. Adding a 6th kind forces
// updating this constant + every `kDirRules` row simultaneously, or the
// static_assert below trips at compile time.
constexpr std::size_t kPackageKindCount = static_cast<std::size_t>(PackageKind::Count);

struct DirRule {
    const char* name;
    bool requiredForKind[kPackageKindCount]; // index by PackageKind
};

static_assert(kPackageKindCount == 5,
    "PackageKind grew or shrank; update every kDirRules row's "
    "requiredForKind initializer before bumping the count");

// Order of PackageKind: Declaration, Layout, EventProtocol, StdlibType, Kernel
const DirRule kDirRules[] = {
    {"declarations", {true, false, false, true, true}},
    {"layouts", {false, true, false, false, true}},
    {"protocols", {false, false, true, false, false}},
    {"impl", {false, false, false, false, true}},
    {"test", {false, false, false, false, true}},
    // migrations/ is always optional — never gates verify.
};

bool dirRequired(const DirRule& rule, PackageKind kind) {
    auto idx = static_cast<std::size_t>(kind);
    // Sentinel `Count` must never reach this path — callers iterate
    // through real PackageKind values only.
    if (idx >= kPackageKindCount) return false;
    return rule.requiredForKind[idx];
}

// ── small helpers ───────────────────────────────────────────────────────

/// True when `dir` holds at least one content entry. A directory that only
/// holds a `.gitkeep` placeholder (written by `tpm init` so empty kind
/// directories survive VCS) counts as empty — verify gates on
/// logical content, not on the placeholder.
bool dirNonEmpty(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return false;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename() == ".gitkeep") continue;
        return true;
    }
    return false;
}

std::string flag(const std::vector<std::string>& args, const std::string& name) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return "";
}

std::vector<std::string> positional(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--from" || a == "--dir" || a == "--kind" || a == "--name" ||
            a == "--version" || a == "--license" || a == "--registry" ||
            a == "--package" || a == "--to" || a == "--source") {
            ++i; // skip the value
            continue;
        }
        if (a.rfind("--", 0) == 0) continue;
        out.push_back(a);
    }
    return out;
}

/// Manifest path for the project / package whose directory is `dir`.
std::string manifestPath(const std::string& dir) {
    return (fs::path(dir) / "tpm.toml").string();
}
std::string lockPath(const std::string& dir) {
    return (fs::path(dir) / "tpm.lock").string();
}

/// Atomic file write: write to ``<path>.tmp`` then rename into place
/// so readers either see the old content or the new content, never a
/// half-written file. The rename is atomic on every POSIX filesystem
/// and on NTFS via ``MoveFileExW`` (which ``fs::rename`` uses on
/// Windows when source and destination are on the same volume — they
/// always are here because both paths share ``p.parent_path()``).
///
/// Returns ``false`` and writes nothing into place on any failure — a
/// temp that could not be opened, a short write (disk full / quota), or
/// a rename+fallback that both failed. The temp file is NEVER renamed
/// over the live file unless its content was confirmed fully written, so
/// a failed write can never replace a good ``tpm.toml`` / ``tpm.lock``
/// with a truncated one. Callers MUST treat ``false`` as a hard error
/// instead of reporting success.
[[nodiscard]] bool writeFile(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false; // temp could not be opened — write nothing.
        out << content;
        out.flush();
        if (!out) { // short write / quota / I/O error — discard the temp.
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        // Fallback for the rare cross-device case (shouldn't happen,
        // same parent dir): copy + remove. The copy_file result is
        // checked — a failure here means nothing was written, so report
        // it rather than swallow it.
        std::error_code copyEc;
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, copyEc);
        fs::remove(tmp, ec);
        if (copyEc) return false;
    }
    return true;
}

/// Path of the project-level tpm lock-file. Acquired by every command
/// that mutates ``tpm.lock`` or ``.topo-pkgs/``. The lock file lives at
/// ``<dir>/.tpm-lock`` and is created if absent. Serializes concurrent
/// writes to the cache and lock-file so parallel tpm runs do not race.
fs::path projectLockPath(const std::string& dir) {
    return fs::path(dir) / ".tpm-lock";
}

/// Acquire the project-level lock, blocking until it is held. Returns
/// ``true`` on success; on failure prints a diagnostic and returns
/// ``false``. The returned ``FileLock`` MUST stay alive for the
/// duration of the mutation (use ``acquireProjectLock`` as a stack
/// guard).
bool acquireProjectLock(::topo::platform::FileLock& lock) {
    if (!lock.ok()) {
        std::cerr << "error: failed to open tpm lock file: "
                  << lock.error() << "\n";
        return false;
    }
    if (!lock.acquire()) {
        std::cerr << "error: failed to acquire tpm lock: "
                  << lock.error() << "\n";
        return false;
    }
    return true;
}

/// Split a dependency token "namespace/name@^1.2" into name + optional req.
void splitPkgSpec(const std::string& spec, std::string& name,
                  std::string& req) {
    size_t at = spec.find('@');
    if (at == std::string::npos) {
        name = spec;
        req = "";
    } else {
        name = spec.substr(0, at);
        req = spec.substr(at + 1);
    }
}

// ── dependency resolution + install (git-based registry) ────────────────

/// Resolve every dependency in `manifest` against the git registry and write
/// the resolved set into `lock`. Returns false on any failure.
///
/// **MVP limitation — single-layer resolution only.** This iterates the
/// direct entries in `manifest.dependencies`. Transitive dependencies
/// (the dependencies of a resolved package, declared in that package's
/// own `tpm.toml`) are NOT enumerated; `tpm.lock` therefore pins only
/// the direct edges of the dependency graph. `installLocked` likewise
/// fetches only what is in the lock, so transitive deps end up missing
/// from the cache. The warning emitted in `installLocked` (see comment
/// there) makes the limitation visible to users until a recursive
/// resolver lands.
bool resolveDependencies(const Manifest& manifest, Lock& lock,
                         std::string& error) {
    GitRegistry registry;
    if (!manifest.dependencies.empty() && !GitRegistry::gitAvailable()) {
        error = "git is required to resolve dependencies but is not on PATH";
        return false;
    }

    Lock fresh;
    for (const auto& dep : manifest.dependencies) {
        // The git URL is the per-dependency `registry` override, else the
        // dependency name is not enough — for the git-based registry the
        // override carries the clone URL.
        std::string repoUrl = dep.registry;
        if (repoUrl.empty()) {
            error = "dependency '" + dep.name +
                    "' has no resolvable git source: provide it as a table "
                    "with an explicit `registry = \"git+...\"` URL";
            return false;
        }
        // Reject option-like / arbitrary-command-transport registry URLs
        // before any `git` subprocess is spawned (git argument injection,
        // CVE-2017-1000117). Defense-in-depth alongside Manifest::validate().
        if (std::string regErr = validateRegistryUrl(repoUrl); !regErr.empty()) {
            error = "dependency '" + dep.name + "' " + regErr;
            return false;
        }
        // Strip an optional `git+` scheme prefix.
        if (repoUrl.rfind("git+", 0) == 0) repoUrl = repoUrl.substr(4);

        auto resolved = registry.resolve(repoUrl, dep.versionReq, error);
        if (!resolved) return false;

        LockedPackage lp;
        lp.name = dep.name;
        lp.version = resolved->version.toString();
        lp.source = repoUrl + "#" + resolved->tag;
        lp.revision = resolved->revision;
        lp.contentHash = ""; // filled at install time once content is on disk
        fresh.packages.push_back(std::move(lp));
    }
    lock = std::move(fresh);
    return true;
}

/// Install every locked package into the cache, fetching from git when the
/// package is not already present. Re-verifies content hashes.
bool installLocked(Lock& lock, const Cache& cache, std::string& error) {
    GitRegistry registry;
    bool anyFetch = false;
    for (auto& lp : lock.packages) {
        if (!lp.source.empty()) anyFetch = true;
    }
    if (anyFetch && !GitRegistry::gitAvailable()) {
        error = "git is required to install packages but is not on PATH";
        return false;
    }

    for (auto& lp : lock.packages) {
        std::string dest = cache.packageDir(lp.name, lp.version);

        // Defence-in-depth: confirm the composed destination stays under the
        // cache root before any fs::remove_all / create_directories touches
        // it. Lock::load already rejects path-traversal in name/version, but
        // installLocked is also reachable with a Lock built by other code
        // paths, so re-affirm containment here. The cache-relative subpath
        // `<name>/<version>` is passed as the input (not the already-rooted
        // `dest`) so the check is correct even when the cache root is a
        // relative path — `dest` is the verbatim write target.
        {
            fs::path subpath = fs::path(lp.name) / lp.version;
            auto sanitised =
                topo::platform::sanitizePath(subpath, fs::path(cache.root()));
            if (!sanitised) {
                error = "locked package '" + lp.name +
                        "' resolves to a destination outside the cache root (" +
                        dest + " not under " + cache.root() +
                        "); rejected as a path-traversal payload";
                return false;
            }
        }

        if (!cache.isInstalled(lp.name, lp.version)) {
            // source form: "<repoUrl>#<tag>"
            size_t hash = lp.source.find('#');
            if (hash == std::string::npos) {
                error = "locked package '" + lp.name +
                        "' has a malformed source '" + lp.source + "'";
                return false;
            }
            std::string repoUrl = lp.source.substr(0, hash);
            std::string tag = lp.source.substr(hash + 1);

            fs::create_directories(fs::path(dest).parent_path());
            // git clone refuses a non-empty destination; clear a stale dir.
            std::error_code ec;
            fs::remove_all(dest, ec);

            std::cout << "  fetching " << lp.name << " " << lp.version
                      << " from " << repoUrl << " @ " << tag << "\n";
            if (!registry.fetchInto(repoUrl, tag, dest, error)) return false;
        } else {
            std::cout << "  cached   " << lp.name << " " << lp.version << "\n";
        }

        std::string actual = computePackageContentHash(dest);
        if (lp.contentHash.empty()) {
            // First resolution — record the hash.
            lp.contentHash = actual;
        } else if (lp.contentHash != actual) {
            error = "content hash mismatch for '" + lp.name + "' " +
                    lp.version + "\n  expected " + lp.contentHash +
                    "\n  actual   " + actual;
            return false;
        }

        // MVP-limit notice: the resolver does not recurse into the
        // installed package's own `tpm.toml`, so any transitive
        // dependency it declares will be silently absent from the
        // cache. Surfacing this at install time gives the user a
        // chance to install them by hand rather than discovering
        // a missing import at consumption time.
        std::string transitiveErr;
        auto installed = Manifest::load(manifestPath(dest), transitiveErr);
        if (installed && !installed->dependencies.empty()) {
            std::cerr << "warning: '" << lp.name << "' " << lp.version
                      << " declares " << installed->dependencies.size()
                      << " dependency(ies) in its tpm.toml; tpm MVP does "
                         "not auto-resolve transitive deps — install them "
                         "by hand if your code consumes them:";
            for (const auto& tdep : installed->dependencies) {
                std::cerr << " " << tdep.name;
            }
            std::cerr << "\n";
        }
    }
    return true;
}

} // namespace

// ── usage ────────────────────────────────────────────────────────────────

void printUsage() {
    std::cout <<
        "tpm — Topo package manager (MVP)\n"
        "\n"
        "Usage: tpm <command> [args]\n"
        "\n"
        "Commands:\n"
        "  init                 Scaffold a new package (tpm.toml + kind dirs)\n"
        "  add <pkg>[@<req>]    Add a dependency, resolve, update tpm.lock\n"
        "  remove <pkg>         Remove a dependency, prune tpm.lock\n"
        "  install              Install what tpm.lock pins (resolve if no lock)\n"
        "  verify               Validate the package against the package format\n"
        "  tree                 Print the resolved dependency graph\n"
        "  publish              Tag the git repo as a published version (stub)\n"
        "  migrate              Apply a package's cross-version migration rules\n"
        "                       to the consumer's .topo files\n"
        "\n"
        "Common flags:\n"
        "  --dir <path>         Operate on this directory (default: cwd)\n"
        "  init    --name N --version V --kind K --license L\n"
        "  add     <pkg>@<req> --registry git+<url>\n"
        "  migrate --package <ns/name> --to <version> [--source <dir>] [--apply]\n"
        "\n"
        "git is required for add / install (a package is a git repo + tag).\n";
}

// ── tpm init ───────────────────────────────────────────────────────────

int cmdInit(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    // Take the project lock BEFORE the existence check so the
    // check-then-write is atomic against a concurrent `tpm init` on the
    // same directory. Without it, two parallel inits both saw "no
    // tpm.toml", both passed the guard, and the second silently clobbered
    // the first's scaffold (TOCTOU). The lock file lives at
    // `<dir>/.tpm-lock`, so the directory must exist before we open it.
    std::error_code mkEc;
    fs::create_directories(dir, mkEc);
    ::topo::platform::FileLock projLock(projectLockPath(dir));
    if (!acquireProjectLock(projLock)) return 1;

    std::string mpath = manifestPath(dir);
    if (fs::exists(mpath)) {
        std::cerr << "error: tpm.toml already exists at " << mpath << "\n";
        return 1;
    }

    Manifest m;
    m.name = flag(args, "--name");
    m.version = flag(args, "--version");
    m.license = flag(args, "--license");
    std::string kindStr = flag(args, "--kind");

    if (m.name.empty()) m.name = "my-org/my-package";
    if (m.version.empty()) m.version = "0.1.0";
    if (m.license.empty()) m.license = "Apache-2.0";
    if (m.coreCompat.empty()) m.coreCompat = ">=4.0.0, <5.0.0";

    if (!kindStr.empty()) {
        auto k = parseKind(kindStr);
        if (!k) {
            std::cerr << "error: invalid --kind '" << kindStr
                      << "' (declaration / layout / event-protocol / "
                         "stdlib-type / kernel)\n";
            return 1;
        }
        m.kind = *k;
    } // else defaults to declaration

    if (!writeFile(mpath, m.toToml())) {
        std::cerr << "error: failed to write " << mpath
                  << " (no changes made)\n";
        return 1;
    }
    std::cout << "created " << mpath << "\n";

    // Create kind-required directories with a .gitkeep so they survive VCS.
    for (const auto& rule : kDirRules) {
        if (dirRequired(rule, m.kind)) {
            fs::path d = fs::path(dir) / rule.name;
            fs::create_directories(d);
            std::ofstream(d / ".gitkeep");
            std::cout << "created " << d.string() << "/\n";
        }
    }

    std::cout << "\nPackage '" << m.name << "' (" << kindToString(m.kind)
              << ") scaffolded. Edit tpm.toml, then run 'tpm verify'.\n";
    return 0;
}

// ── tpm add ────────────────────────────────────────────────────────────

int cmdAdd(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    auto pos = positional(args);
    if (pos.empty()) {
        std::cerr << "error: tpm add requires a package spec "
                     "(<namespace>/<name>[@<req>])\n";
        return 1;
    }

    // Project-level file lock: serialise concurrent tpm invocations
    // that all want to mutate ``tpm.lock`` / ``.topo-pkgs/``. Blocks
    // until acquired.
    ::topo::platform::FileLock projLock(projectLockPath(dir));
    if (!acquireProjectLock(projLock)) return 1;

    std::string name, req;
    splitPkgSpec(pos[0], name, req);
    if (req.empty()) req = "*";

    std::string registryUrl = flag(args, "--registry");

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    Manifest m = *loaded;

    // Replace an existing entry, else append.
    bool replaced = false;
    for (auto& dep : m.dependencies) {
        if (dep.name == name) {
            dep.versionReq = req;
            if (!registryUrl.empty()) dep.registry = registryUrl;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        Dependency dep;
        dep.name = name;
        dep.versionReq = req;
        dep.registry = registryUrl;
        m.dependencies.push_back(std::move(dep));
    }

    if (!writeFile(manifestPath(dir), m.toToml())) {
        std::cerr << "error: failed to write " << manifestPath(dir)
                  << " (no changes made)\n";
        return 1;
    }
    std::cout << (replaced ? "updated" : "added") << " dependency " << name
              << " = \"" << req << "\"\n";

    // Re-resolve and rewrite the lock. The manifest now carries `name` but
    // the on-disk tpm.lock does not yet pin it, so any early return below
    // leaves the project in a manifest-ahead-of-lock state. Surface that
    // stale-lock condition explicitly — "left untouched" alone reads as
    // "everything is fine," when in fact the lock no longer matches the
    // manifest and the user must re-run `tpm add` / `tpm install` to
    // reconcile (or `tpm remove` to undo the manifest edit).
    auto warnStaleLock = [&]() {
        std::cerr << "warning: tpm.lock is now STALE — the manifest gained '"
                  << name << "' but the lock was not updated. Re-run 'tpm "
                     "add "
                  << name << "' or 'tpm install' once the cause above is "
                     "resolved, or 'tpm remove "
                  << name << "' to revert the manifest edit.\n";
    };
    Lock lock;
    if (!resolveDependencies(m, lock, err)) {
        std::cerr << "error: resolution failed: " << err << "\n";
        warnStaleLock();
        return 1;
    }
    Cache cache(dir);
    if (!installLocked(lock, cache, err)) {
        std::cerr << "error: install failed: " << err << "\n";
        warnStaleLock();
        return 1;
    }
    if (!writeFile(lockPath(dir), lock.toToml())) {
        std::cerr << "error: failed to write " << lockPath(dir) << "\n";
        warnStaleLock();
        return 1;
    }
    std::cout << "updated tpm.lock (" << lock.packages.size()
              << " package(s) pinned)\n";
    return 0;
}

// ── tpm remove ─────────────────────────────────────────────────────────

int cmdRemove(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    auto pos = positional(args);
    if (pos.empty()) {
        std::cerr << "error: tpm remove requires a package name\n";
        return 1;
    }
    const std::string& name = pos[0];

    // Project-level file lock — see cmdAdd for rationale.
    ::topo::platform::FileLock projLock(projectLockPath(dir));
    if (!acquireProjectLock(projLock)) return 1;

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    Manifest m = *loaded;

    size_t before = m.dependencies.size();
    m.dependencies.erase(
        std::remove_if(m.dependencies.begin(), m.dependencies.end(),
                       [&](const Dependency& d) { return d.name == name; }),
        m.dependencies.end());
    if (m.dependencies.size() == before) {
        std::cerr << "error: '" << name << "' is not a dependency\n";
        return 1;
    }

    if (!writeFile(manifestPath(dir), m.toToml())) {
        std::cerr << "error: failed to write " << manifestPath(dir)
                  << " (no changes made)\n";
        return 1;
    }
    std::cout << "removed dependency " << name << "\n";

    // Re-resolve so the lock prunes the removed package and its now-orphaned
    // transitive deps.
    Lock lock;
    if (!resolveDependencies(m, lock, err)) {
        std::cerr << "warning: re-resolution failed (" << err
                  << "); pruning lock entry only\n";
        bool exists = false;
        auto existing = Lock::load(lockPath(dir), exists, err);
        if (existing) {
            existing->packages.erase(
                std::remove_if(existing->packages.begin(),
                               existing->packages.end(),
                               [&](const LockedPackage& p) {
                                   return p.name == name;
                               }),
                existing->packages.end());
            if (!writeFile(lockPath(dir), existing->toToml())) {
                std::cerr << "error: failed to write " << lockPath(dir)
                          << " (manifest updated; tpm.lock left stale)\n";
                return 1;
            }
        }
        return 0;
    }
    Cache cache(dir);
    if (!installLocked(lock, cache, err)) {
        std::cerr << "warning: install of remaining deps failed: " << err << "\n";
    }
    if (lock.packages.empty()) {
        std::error_code ec;
        fs::remove(lockPath(dir), ec);
        std::cout << "tpm.lock removed (no dependencies remain)\n";
    } else {
        if (!writeFile(lockPath(dir), lock.toToml())) {
            std::cerr << "error: failed to write " << lockPath(dir)
                      << " (manifest updated; tpm.lock left stale)\n";
            return 1;
        }
        std::cout << "updated tpm.lock (" << lock.packages.size()
                  << " package(s) pinned)\n";
    }
    return 0;
}

// ── tpm install ────────────────────────────────────────────────────────

int cmdInstall(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    // Project-level file lock — see cmdAdd for rationale. install is
    // the primary contention point: editor-save concurrent with a CLI
    // ``tpm install`` and parallel CI builds sharing a cache directory
    // all race on ``.topo-pkgs/`` writes.
    ::topo::platform::FileLock projLock(projectLockPath(dir));
    if (!acquireProjectLock(projLock)) return 1;

    std::string fromDir = flag(args, "--from");
    if (!fromDir.empty()) {
        // Offline install-from-directory (the `tpm pack` consumption path).
        std::string err;
        auto srcManifest = Manifest::load(manifestPath(fromDir), err);
        if (!srcManifest) {
            std::cerr << "error: " << err << "\n";
            return 1;
        }
        auto problems = srcManifest->validate();
        if (!problems.empty()) {
            std::cerr << "error: package at " << fromDir
                      << " fails manifest validation:\n";
            for (const auto& p : problems) std::cerr << "  - " << p << "\n";
            return 1;
        }
        Cache cache(dir);
        std::string dest =
            cache.packageDir(srcManifest->name, srcManifest->version);

        // Audit: untrusted manifest fields enabling path traversal.
        // Manifest::validate() already rejects path-separator and ``..``
        // payloads in version, but the dest is still composed from
        // untrusted name + version fields. Defence-in-depth: confirm
        // the composed path stays under the cache root before writing.
        // This is a second line, not the primary defence — the primary
        // defence is validate()'s reject at parse time.
        //
        // sanitizePath joins a RELATIVE `input` onto `root` before the
        // containment check, so the input must be the cache-relative
        // `<name>/<version>` subpath — NOT the already-rooted `dest`.
        // Passing `dest` (which is `<dir>/.topo-pkgs/<name>/<version>`)
        // when `--dir` is relative (the default `.`) made sanitizePath
        // validate a doubled, fictional `<root>/<root>/<name>/<version>`
        // path: the guard checked the wrong string and a traversal payload
        // in name/version was measured against the doubled prefix. Mirror
        // installLocked: pass the subpath, keep `dest` as the verbatim
        // write target.
        fs::path cacheRoot(cache.root());
        fs::path subpath = fs::path(srcManifest->name) / srcManifest->version;
        auto sanitised = topo::platform::sanitizePath(subpath, cacheRoot);
        if (!sanitised) {
            std::cerr << "error: package at " << fromDir
                      << " resolves to a destination outside the cache "
                      << "root (" << dest << " ⊄ " << cacheRoot << "); "
                      << "rejected as a path-traversal payload.\n";
            return 1;
        }

        // Atomic install: copy to a sibling ``.tmp`` directory first,
        // then ``fs::rename`` into place. A partial copy failure leaves
        // the temp tree behind (which we explicitly remove); the live
        // ``dest`` either matches the source completely or does not
        // exist. Without this, a transient mid-copy error left ``dest``
        // half-populated and ``Cache::isInstalled`` (which only checks
        // for ``tpm.toml``) would report the broken cache as installed
        // — so a re-run silently skipped re-installation.
        fs::path destPath(dest);
        fs::path tmpDest = destPath;
        tmpDest += ".install.tmp";

        std::error_code ec;
        fs::remove_all(tmpDest, ec);
        fs::create_directories(destPath.parent_path(), ec);
        // Audit: untrusted source directory enabling path traversal.
        // without ``copy_symlinks`` ``fs::copy`` dereferences source
        // symlinks, so a hostile ``--from`` directory pointing at
        // ``/etc/passwd`` would have its content silently copied into
        // the cache. Preserve symlinks verbatim instead — the cache
        // either records "this entry is a symlink" or fails out, and
        // downstream consumers' own sanitisation decides what to do
        // with it.
        fs::copy(fromDir, tmpDest,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 ec);
        if (ec) {
            std::cerr << "error: failed to copy package: " << ec.message()
                      << "\n";
            // Roll back: remove the half-populated temp tree so a
            // re-run starts clean. Errors here are not fatal — the
            // user is already being told the install failed.
            std::error_code cleanupEc;
            fs::remove_all(tmpDest, cleanupEc);
            return 1;
        }

        // Atomically swap: ``dest`` is gone before the rename, so the
        // rename either succeeds (cache complete) or leaves the cache
        // in its prior state. We cannot use ``fs::rename`` directly to
        // overlay a non-empty destination, so the prior cached version
        // is removed in a separate step under the parent directory.
        fs::remove_all(destPath, ec);
        if (ec) {
            std::cerr << "error: failed to remove prior cache entry "
                      << destPath.string() << ": " << ec.message() << "\n";
            std::error_code cleanupEc;
            fs::remove_all(tmpDest, cleanupEc);
            return 1;
        }
        fs::rename(tmpDest, destPath, ec);
        if (ec) {
            std::cerr << "error: failed to finalise install (rename "
                      << tmpDest.string() << " -> " << destPath.string()
                      << "): " << ec.message() << "\n";
            std::error_code cleanupEc;
            fs::remove_all(tmpDest, cleanupEc);
            return 1;
        }

        std::cout << "installed " << srcManifest->name << " "
                  << srcManifest->version << " from " << fromDir << "\n";
        return 0;
    }

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    const Manifest& m = *loaded;

    bool lockExists = false;
    auto lockOpt = Lock::load(lockPath(dir), lockExists, err);
    if (!lockOpt) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    Lock lock = *lockOpt;

    if (!lockExists) {
        // No lock: resolve, install, write the lock.
        std::cout << "no tpm.lock — resolving [dependencies]...\n";
        if (!resolveDependencies(m, lock, err)) {
            std::cerr << "error: resolution failed: " << err << "\n";
            return 1;
        }
    } else {
        std::cout << "installing from tpm.lock (" << lock.packages.size()
                  << " package(s))...\n";
    }

    Cache cache(dir);
    // Snapshot the serialized lock so a first-seen content hash filled in by
    // installLocked gets persisted even when a lock already existed — without
    // this the freshly computed TOFU pin is silently discarded and `tpm
    // verify` keeps skipping the integrity check for that package.
    std::string lockBefore = lock.toToml();
    if (!installLocked(lock, cache, err)) {
        std::cerr << "error: install failed: " << err << "\n";
        return 1;
    }

    if (!lockExists || lock.toToml() != lockBefore) {
        if (!writeFile(lockPath(dir), lock.toToml())) {
            std::cerr << "error: failed to write " << lockPath(dir)
                      << " (packages installed; tpm.lock not updated)\n";
            return 1;
        }
    }

    std::cout << "install complete — cache at " << cache.root() << "\n";
    return 0;
}

// ── tpm verify ─────────────────────────────────────────────────────────

int cmdVerify(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    const Manifest& m = *loaded;

    std::vector<std::string> hardFailures;
    std::vector<std::string> warnings;

    // Manifest schema.
    for (auto& p : m.validate()) hardFailures.push_back("manifest: " + p);

    // kind-required directories present and non-empty.
    for (const auto& rule : kDirRules) {
        if (dirRequired(rule, m.kind)) {
            fs::path d = fs::path(dir) / rule.name;
            if (!dirNonEmpty(d))
                hardFailures.push_back(
                    std::string("kind '") + kindToString(m.kind) +
                    "' requires a non-empty '" + rule.name + "/' directory");
        }
    }

    // tpm.lock consistency: every locked package's content hash still matches.
    bool lockExists = false;
    auto lockOpt = Lock::load(lockPath(dir), lockExists, err);
    if (!lockOpt) {
        hardFailures.push_back(err);
    } else if (lockExists) {
        Cache cache(dir);
        for (const auto& lp : lockOpt->packages) {
            std::string pdir = cache.packageDir(lp.name, lp.version);
            if (!fs::exists(pdir)) {
                warnings.push_back("locked package '" + lp.name + "' " +
                                   lp.version +
                                   " is not installed (run 'tpm install')");
                continue;
            }
            std::string actual = computePackageContentHash(pdir);
            if (!lp.contentHash.empty() && lp.contentHash != actual)
                hardFailures.push_back("content hash mismatch for locked '" +
                                       lp.name + "' " + lp.version);
        }
    }
    if (!lockExists && !m.dependencies.empty())
        warnings.push_back("[dependencies] present but no tpm.lock — run "
                           "'tpm install'");

    // [bindings] advisory presence — never fails, only a note.
    if (!m.bindings.empty())
        warnings.push_back(std::to_string(m.bindings.size()) +
                           " advisory host binding(s) declared; tpm does not "
                           "install them");

    for (const auto& w : warnings)
        std::cout << "warning: " << w << "\n";
    for (const auto& f : hardFailures)
        std::cerr << "error: " << f << "\n";

    if (!hardFailures.empty()) {
        std::cerr << "verify FAILED (" << hardFailures.size()
                  << " hard failure(s))\n";
        return 1;
    }
    std::cout << "verify OK — package '" << m.name << "' "
              << m.version << " (" << kindToString(m.kind)
              << ") conforms to the package format\n";
    return 0;
}

// ── tpm tree ───────────────────────────────────────────────────────────

int cmdTree(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    const Manifest& m = *loaded;

    std::cout << m.name << " " << m.version << " (" << kindToString(m.kind)
              << ")\n";

    bool lockExists = false;
    auto lockOpt = Lock::load(lockPath(dir), lockExists, err);

    if (m.dependencies.empty()) {
        std::cout << "  (no dependencies)\n";
        return 0;
    }

    for (size_t i = 0; i < m.dependencies.size(); ++i) {
        const auto& dep = m.dependencies[i];
        bool last = (i + 1 == m.dependencies.size());
        std::cout << (last ? "  └── " : "  ├── ") << dep.name << " "
                  << dep.versionReq;
        if (lockExists && lockOpt) {
            if (const LockedPackage* lp = lockOpt->find(dep.name)) {
                std::cout << "  → " << lp->version;
                if (!lp->revision.empty())
                    std::cout << " (" << lp->revision.substr(0, 12) << ")";
            } else {
                std::cout << "  (unresolved)";
            }
        } else {
            std::cout << "  (no lock)";
        }
        std::cout << "\n";
    }
    return 0;
}

// ── tpm publish — stub (MVP gap) ───────────────────────────────────────

int cmdPublish(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    // publish must run verify first.
    int vrc = cmdVerify({"--dir", dir});
    if (vrc != 0) {
        std::cerr << "publish refused: package fails 'tpm verify'\n";
        return vrc;
    }

    std::string err;
    auto loaded = Manifest::load(manifestPath(dir), err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    std::cerr << "tpm publish is a STUB in this MVP.\n"
                 "  The package verifies; the git-tag / central-registry "
                 "upload step is not implemented.\n"
                 "  To publish manually with the git-based registry:\n"
                 "    git tag "
              << loaded->version << " && git push origin " << loaded->version
              << "\n";
    return 2; // non-zero: the operation did not complete
}

// ── tpm migrate ────────────────────────────────────────────────────────
//
// Applies a package's cross-version declaration-migration rules to the
// consumer's `.topo` files.
//
//   tpm migrate --package <ns/name> --to <version> [--source <dir>] [--apply]
//
// The rule files come from the installed package's `migrations/` directory.
// The package's *current* version is the version `tpm.lock` pins.
// `--source` selects the directory whose `.topo` files are migrated
// (default: the project root). Without `--apply` the command is a dry run:
// it prints the migration report but does not write any file.

namespace {

/// Case-insensitive equality for filesystem path segments. The audit
/// finding: on Windows / macOS HFS+ the filesystem is case-insensitive
/// by default, so a ``.Topo-Pkgs/`` checkout would escape a case-sensitive
/// exclusion. On Linux the same path comparison stays strict, so a
/// distinct lowercase / uppercase pair on a case-sensitive filesystem
/// still compares unequal.
bool segmentEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
#ifdef _WIN32
    constexpr bool caseInsensitive = true;
#elif defined(__APPLE__)
    // HFS+ / APFS default to case-insensitive comparisons.
    constexpr bool caseInsensitive = true;
#else
    constexpr bool caseInsensitive = false;
#endif
    if (!caseInsensitive) return a == b;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return false;
    }
    return true;
}

/// Test whether ``candidate`` lies under a directory named exactly
/// ``.topo-pkgs`` (relative to ``root``). Replaces the previous
/// ``s.find(".topo-pkgs")`` substring scan, which had two bugs:
///   - case-sensitive on Windows / macOS HFS+ (``.Topo-Pkgs/`` escaped)
///   - substring match (``my-app-.topo-pkgs-design/`` falsely matched)
bool isInsideTopoPkgs(const fs::path& candidate, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::relative(candidate, root, ec);
    if (ec) return false; // unrelated path: not under root
    for (const auto& seg : rel) {
        if (segmentEquals(seg.string(), ".topo-pkgs")) return true;
    }
    return false;
}

/// Collect every `.topo` file under `dir` (recursive).
std::vector<fs::path> collectTopoFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) return out;
    if (fs::is_regular_file(dir)) {
        if (dir.extension() == ".topo") out.push_back(dir);
        return out;
    }
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".topo") {
            // Skip the package cache itself — it carries the *library's*
            // declarations, not the consumer's code. ``isInsideTopoPkgs``
            // checks for the segment exactly (no substring false-positives
            // such as ``my-app-.topo-pkgs-design/``) and case-insensitively
            // on case-insensitive filesystems (Windows / macOS default).
            if (isInsideTopoPkgs(e.path(), dir)) continue;
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int cmdMigrate(const std::vector<std::string>& args) {
    std::string dir = flag(args, "--dir");
    if (dir.empty()) dir = ".";

    std::string pkgName = flag(args, "--package");
    std::string toVersion = flag(args, "--to");
    std::string sourceDir = flag(args, "--source");
    if (sourceDir.empty()) sourceDir = dir;
    bool apply = std::find(args.begin(), args.end(), "--apply") != args.end();

    // For --apply we mutate consumer .topo files in-place under
    // `sourceDir`, so the lock that serialises against concurrent tpm
    // runs MUST cover `sourceDir` — the directory that actually receives
    // the writes — not just `dir`. When `--source` differs from `--dir`
    // (e.g. migrating a sibling source tree) locking only `dir` left the
    // real writes unserialised. We lock `sourceDir`; when `dir` differs we
    // also lock `dir` (it owns the `tpm.lock` we read) and acquire both in
    // a deterministic path order so two concurrent runs can never deadlock
    // ABBA. Dry-run mode (no --apply) is read-only and stays lock-free.
    std::error_code lockEc;
    fs::path dirLockPath = projectLockPath(dir);
    fs::path srcLockPath = projectLockPath(sourceDir);
    bool sameLock = fs::equivalent(dir, sourceDir, lockEc) ||
                    dirLockPath == srcLockPath;
    // Order the two lock paths so every caller takes them in the same
    // sequence (string order is a total order over the path strings).
    std::vector<fs::path> lockOrder;
    if (sameLock) {
        lockOrder.push_back(srcLockPath);
    } else if (dirLockPath.string() < srcLockPath.string()) {
        lockOrder.push_back(dirLockPath);
        lockOrder.push_back(srcLockPath);
    } else {
        lockOrder.push_back(srcLockPath);
        lockOrder.push_back(dirLockPath);
    }
    std::vector<std::unique_ptr<::topo::platform::FileLock>> heldLocks;
    if (apply) {
        for (const auto& lp : lockOrder) {
            auto lock = std::make_unique<::topo::platform::FileLock>(lp);
            if (!acquireProjectLock(*lock)) return 1;
            heldLocks.push_back(std::move(lock));
        }
    }

    if (pkgName.empty() || toVersion.empty()) {
        std::cerr << "error: tpm migrate requires --package <ns/name> and "
                     "--to <version>\n"
                     "  usage: tpm migrate --package <ns/name> --to <version> "
                     "[--source <dir>] [--apply]\n";
        return 1;
    }

    // The package's current (locked) version is the migration's start point.
    std::string err;
    bool lockExists = false;
    auto lockOpt = Lock::load(lockPath(dir), lockExists, err);
    if (!lockOpt) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (!lockExists) {
        std::cerr << "error: no tpm.lock — cannot determine the currently "
                     "locked version of '" << pkgName << "' to migrate from. "
                     "Run 'tpm install' first.\n";
        return 1;
    }
    const LockedPackage* locked = lockOpt->find(pkgName);
    if (!locked) {
        std::cerr << "error: '" << pkgName
                  << "' is not a locked dependency of this project\n";
        return 1;
    }
    std::string fromVersion = locked->version;

    // Locate the package's migrations/ directory inside the install cache.
    Cache cache(dir);
    fs::path pkgDir = cache.packageDir(pkgName, fromVersion);
    fs::path migrationsDir = pkgDir / "migrations";
    fs::path indexPath = migrationsDir / "index.toml";

    bool indexExists = false;
    auto indexOpt = MigrationIndex::load(indexPath.string(), indexExists, err);
    if (!indexOpt) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (!indexExists || indexOpt->entries.empty()) {
        std::cout << "package '" << pkgName << "' carries no migration rules "
                     "(no migrations/index.toml) — nothing to migrate.\n";
        return 0;
    }

    // Select the continuous chain of migration steps.
    auto pathOpt = indexOpt->selectPath(fromVersion, toVersion, err);
    if (!pathOpt) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (pathOpt->empty()) {
        std::cout << "'" << pkgName << "' is already at " << toVersion
                  << " — nothing to migrate.\n";
        return 0;
    }

    std::cout << "migrating consumer .topo against '" << pkgName << "' "
              << fromVersion << " -> " << toVersion << " ("
              << pathOpt->size() << " step(s))\n";

    auto topoFiles = collectTopoFiles(fs::path(sourceDir));
    if (topoFiles.empty()) {
        std::cout << "no .topo files found under " << sourceDir
                  << " — nothing to migrate.\n";
        return 0;
    }

    MigrationEngine engine;
    int totalAuto = 0, totalManual = 0;
    bool hardError = false;

    // Per-file in-memory cursor. The output of step N is the input of
    // step N+1, so we keep each file's current text in memory
    // and feed it forward across steps. Without this, dry-run reads
    // the on-disk source every step, dropping any changes step N
    // would have committed had `--apply` been passed — so the preview
    // for step N+1 is wrong. (Apply mode also benefits: a single
    // batched read per file.)
    std::map<std::string, std::string> cursor;
    // Snapshot of the on-disk contents the FIRST time `--apply` is
    // about to overwrite a given file. If a later step hard-fails we
    // roll every snapshotted file back, so a mid-migration crash
    // never leaves the consumer with a partially-migrated tree.
    std::map<std::string, std::string> originals;

    // Each step's rules are applied in `to`-ascending order; the output of
    // one step is the input of the next. Every step verifies
    // independently via the dual-contract verifier inside the engine.
    for (const auto& step : *pathOpt) {
        fs::path rulesPath = migrationsDir / step.rulesFile;
        auto rulesOpt = MigrationRuleSet::load(rulesPath.string(), err);
        if (!rulesOpt) {
            std::cerr << "error: " << err << "\n";
            hardError = true;
            break;
        }
        std::cout << "  step -> " << step.toVersion << "  ("
                  << rulesOpt->rules.size() << " rule(s))\n";

        for (const auto& tf : topoFiles) {
            std::string tfKey = tf.string();
            auto cit = cursor.find(tfKey);
            if (cit == cursor.end()) {
                cit = cursor.emplace(tfKey, readWholeFile(tf)).first;
            }
            // Take `src` by value (not by reference into the cursor
            // map slot) so the rollback-snapshot below captures the
            // pre-step text even after we mutate cursor[tfKey].
            std::string src = cit->second;
            auto res = engine.migrateSource(tfKey, src, *rulesOpt);
            if (!res.ok) {
                std::cerr << "error: " << res.error << "\n";
                hardError = true;
                continue;
            }
            for (const auto& e : res.report) {
                bool auto_ = e.outcome ==
                             MigrationReportEntry::Outcome::Auto;
                std::cout << "    " << (auto_ ? "[auto]  " : "[manual]")
                          << " " << e.location << "  " << e.declaration
                          << "  (" << e.rule << ")";
                if (!auto_) std::cout << "\n             reason: " << e.reason;
                std::cout << "\n";
                if (auto_) ++totalAuto;
                else ++totalManual;
            }
            if (res.changed) {
                if (apply) {
                    // Snapshot the pre-migration on-disk text the first
                    // time we are about to write this file, so a later
                    // hard-error can roll it back. The snapshot must
                    // be the text BEFORE this step's rewrite — which
                    // is what `src` carries because we copied it above
                    // before updating the cursor.
                    if (originals.find(tfKey) == originals.end()) {
                        originals[tfKey] = src;
                    }
                    if (!writeFile(tf, res.rewrittenSource)) {
                        std::cerr << "error: failed to write " << tf.string()
                                  << " during migration\n";
                        hardError = true;
                        break;
                    }
                    std::cout << "    wrote " << tf.string() << "\n";
                } else {
                    std::cout << "    (dry run — " << tf.string()
                              << " would be rewritten; pass --apply to "
                                 "commit)\n";
                }
            }
            // Thread the rewrite forward regardless of apply mode, so
            // step N+1's input is what step N produced.
            cursor[tfKey] = res.rewrittenSource;
        }
        if (hardError) break;
    }

    // Roll back any apply-mode writes made before the hard error fired.
    // Dry-run never touches disk, so `originals` is empty there.
    if (hardError && apply && !originals.empty()) {
        size_t rolled = 0;
        for (const auto& [path, content] : originals) {
            // Route the restore through the atomic writeFile (tmp + rename)
            // so a rollback is all-or-nothing — a direct truncating open
            // could leave a consumer `.topo` half-written, the exact state
            // rollback exists to prevent.
            if (writeFile(fs::path(path), content)) ++rolled;
        }
        if (rolled < originals.size())
            std::cerr << "error: rollback incomplete — " << rolled << " of "
                      << originals.size() << " file(s) restored; the "
                      << "remaining file(s) may be left mid-migration\n";
        std::cerr << "error: rolled back " << rolled
                  << " file(s) due to mid-migration failure\n";
    }

    std::cout << "\nmigration summary: " << totalAuto << " auto, "
              << totalManual << " manual";
    if (!apply) std::cout << "  (dry run)";
    std::cout << "\n";

    // Exit code: hard error → 1; any manual outcome → non-zero;
    // all auto → 0.
    if (hardError) return 1;
    if (totalManual > 0) {
        std::cout << totalManual
                  << " call site(s) need manual migration — see the "
                     "[manual] entries above.\n";
        return 3;
    }
    return 0;
}

} // namespace tpm
