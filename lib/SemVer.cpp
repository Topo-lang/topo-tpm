#include "tpm/SemVer.h"

#include "semver.hpp"

#include <cctype>
#include <limits>
#include <sstream>

// Why this file is only a PARTIAL adoption of Neargye/semver
// =============================================================
// The open-source-reuse plan originally proposed dropping all of this
// file in favour of `Neargye/semver`. After integration we kept three
// pieces native; this header records why so a future cleanup doesn't
// re-attempt the same replacement.
//
// 1. SemVer::parse — only the numeric `major.minor.patch` core is fed
//    to `semver::parse`. The prerelease field stays raw (see
//    `splitVersion` comment below).  Neargye's lexer rejects any byte
//    outside `[0-9A-Za-z-]` in the prerelease, but tpm intentionally
//    accepts arbitrary bytes there so `Manifest::validate` can flag
//    path-traversal payloads with a clear error.
//
// 2. SemVer::compare — the prerelease portion is compared lexically.
//    semver.org §11 specifies an identifier-by-identifier rule
//    (numeric < alphanumeric, dot-separated), which Neargye implements
//    correctly — but only against spec-valid prereleases. Since tpm's
//    prerelease can contain bytes Neargye refuses, we can't always
//    round-trip through `semver::version` for comparison, and a
//    lexical fallback is what tpm has always done.
//
// 3. VersionReq — Neargye/semver's `range_set` grammar covers
//    `<`/`<=`/`>`/`>=`/`=`/`||`/space, but does NOT recognise the
//    Cargo-style `^` (caret) or `~` (tilde) range operators or the
//    comma-as-AND separator that tpm's manifests, `tpm.lock`, and
//    migration rules all use today (e.g. `>=4.0.0, <5.0.0`, `^1.2`,
//    `~1.2.3`). The native dispatcher below translates those into
//    explicit lower/upper bounds; replacing it would require either
//    a rewrite of the entire range surface to space-separated AND or
//    a pre-pass that desugars `^`/`~`/`,` before feeding `range_set`,
//    neither of which is shorter than the existing code.

namespace tpm {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Split into (major.minor.patch, raw-prerelease).
//
// tpm intentionally accepts arbitrary bytes in the prerelease field — the
// security model is parse-permissive, validate-strict: malicious payloads
// like ``1.0.0-../../etc/passwd`` MUST flow through parse so
// Manifest::validate() can reject them with a clear error rather than
// getting silently swallowed here. So we only let semver.hpp see the
// numeric core, never the prerelease.
struct Split {
    std::string core;
    std::string prerelease; // empty when none; raw, not validated
};
std::optional<Split> splitVersion(const std::string& input) {
    std::string t = trim(input);
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t = t.substr(1);
    if (t.empty()) return std::nullopt;

    Split s;
    size_t dash = t.find('-');
    if (dash != std::string::npos) {
        s.prerelease = t.substr(dash + 1);
        s.core = t.substr(0, dash);
    } else {
        s.core = t;
    }
    // tpm discards build metadata; strip ``+...`` from the core.
    size_t plus = s.core.find('+');
    if (plus != std::string::npos) s.core = s.core.substr(0, plus);

    // Pad short forms (``1`` / ``1.2``) so the strict semver.hpp parser
    // accepts them.
    int dots = 0;
    for (char c : s.core) {
        if (c == '.') ++dots;
        else if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    if (s.core.empty() || s.core.front() == '.' || s.core.back() == '.') return std::nullopt;
    while (dots < 2) { s.core += ".0"; ++dots; }
    if (dots > 2) return std::nullopt;
    return s;
}

} // namespace

std::optional<SemVer> SemVer::parse(const std::string& text) {
    auto s = splitVersion(text);
    if (!s) return std::nullopt;
    semver::version<int, int, int> v;
    if (!semver::parse(s->core, v)) return std::nullopt;
    SemVer r;
    r.major = v.major();
    r.minor = v.minor();
    r.patch = v.patch();
    r.prerelease = s->prerelease;
    return r;
}

std::string SemVer::toString() const {
    std::ostringstream os;
    os << major << '.' << minor << '.' << patch;
    if (!prerelease.empty()) os << '-' << prerelease;
    return os.str();
}

int SemVer::compare(const SemVer& other) const {
    if (major != other.major) return major < other.major ? -1 : 1;
    if (minor != other.minor) return minor < other.minor ? -1 : 1;
    if (patch != other.patch) return patch < other.patch ? -1 : 1;
    // A version with a pre-release is lower than one without. Prereleases
    // themselves are compared lexically — the rigorous semver.org §11
    // identifier-by-identifier rule isn't worth the dependency here and
    // tpm's prerelease field is permissive anyway (may contain bytes
    // semver.hpp would reject).
    if (prerelease.empty() != other.prerelease.empty())
        return prerelease.empty() ? 1 : -1;
    if (prerelease != other.prerelease)
        return prerelease < other.prerelease ? -1 : 1;
    return 0;
}

// Native VersionReq dispatcher. See the file header for why this is
// NOT a wrapper around `semver::range_set`: Cargo-style `^`/`~` and
// comma-as-AND aren't in Neargye's range grammar, and tpm manifests
// already use them.
std::optional<VersionReq> VersionReq::parse(const std::string& text) {
    VersionReq req;
    req.raw_ = trim(text);
    if (req.raw_.empty()) return std::nullopt;

    std::stringstream ss(req.raw_);
    std::string part;
    while (std::getline(ss, part, ',')) {
        std::string p = trim(part);
        if (p.empty()) continue;
        Clause clause;
        size_t verStart = 0;
        if (p == "*") {
            clause.op = Clause::Op::Any;
            req.clauses_.push_back(clause);
            continue;
        } else if (p.rfind(">=", 0) == 0) {
            clause.op = Clause::Op::Gte;
            verStart = 2;
        } else if (p.rfind("<=", 0) == 0) {
            clause.op = Clause::Op::Lte;
            verStart = 2;
        } else if (p.rfind('>', 0) == 0) {
            clause.op = Clause::Op::Gt;
            verStart = 1;
        } else if (p.rfind('<', 0) == 0) {
            clause.op = Clause::Op::Lt;
            verStart = 1;
        } else if (p.rfind('^', 0) == 0) {
            clause.op = Clause::Op::Caret;
            verStart = 1;
        } else if (p.rfind('~', 0) == 0) {
            clause.op = Clause::Op::Tilde;
            verStart = 1;
        } else if (p.rfind('=', 0) == 0) {
            clause.op = Clause::Op::Eq;
            verStart = 1;
        } else {
            // Bare version. `1.2.3` is exact; `1.2` / `1` are caret-style.
            int dots = 0;
            for (char c : p)
                if (c == '.') ++dots;
            clause.op = (dots >= 2) ? Clause::Op::Eq : Clause::Op::Caret;
        }
        std::string verText = trim(p.substr(verStart));
        auto v = SemVer::parse(verText);
        if (!v) return std::nullopt;
        clause.bound = *v;
        // Record how many components the user actually wrote, before
        // splitVersion's padding erases the distinction. The caret/tilde
        // upper bound depends on it (`~1` vs `~1.0`, `^0` vs `^0.0.0`).
        {
            std::string core = verText;
            if (!core.empty() && (core[0] == 'v' || core[0] == 'V'))
                core = core.substr(1);
            size_t dash = core.find('-');
            if (dash != std::string::npos) core = core.substr(0, dash);
            size_t plus = core.find('+');
            if (plus != std::string::npos) core = core.substr(0, plus);
            int dots = 0;
            for (char c : core)
                if (c == '.') ++dots;
            clause.components = dots + 1; // 1 dot => 2 components, etc.
            if (clause.components > 3) clause.components = 3;
        }
        req.clauses_.push_back(clause);
    }
    if (req.clauses_.empty()) return std::nullopt;
    return req;
}

namespace {
// Bump a component for an upper bound without signed-overflow UB. A
// component already at INT_MAX cannot grow; treat that as an effectively
// unbounded ceiling (INT_MAX) rather than wrapping to a negative value.
int bumpForUpper(int x) {
    return x == std::numeric_limits<int>::max()
               ? std::numeric_limits<int>::max()
               : x + 1;
}
} // namespace

bool VersionReq::matches(const SemVer& v) const {
    for (const auto& c : clauses_) {
        const SemVer& b = c.bound;
        switch (c.op) {
        case Clause::Op::Any:
            break;
        case Clause::Op::Eq:
            if (!(v == b)) return false;
            break;
        case Clause::Op::Gt:
            if (!(b < v)) return false;
            break;
        case Clause::Op::Gte:
            if (v < b) return false;
            break;
        case Clause::Op::Lt:
            if (!(v < b)) return false;
            break;
        case Clause::Op::Lte:
            if (b < v) return false;
            break;
        case Clause::Op::Caret: {
            // ^1.2.3 := >=1.2.3, <2.0.0 ; ^0.3 := >=0.3.0, <0.4.0 ;
            // ^0.0.x := >=0.0.x, <0.0.(x+1) ; ^0 := >=0.0.0, <1.0.0 .
            // A leading-zero major (`^0`) only widens to `<1.0.0` when the
            // user wrote major only; `^0.0.0` stays `<0.0.1`. `c.components`
            // carries the originally specified width before padding.
            if (v < b) return false;
            SemVer upper = b;
            if (b.major > 0) {
                upper.major = bumpForUpper(b.major);
                upper.minor = 0;
                upper.patch = 0;
            } else if (b.minor > 0) {
                upper.minor = bumpForUpper(b.minor);
                upper.patch = 0;
            } else if (c.components <= 1) {
                // ^0 — a zero major with no minor/patch given allows the
                // whole 0.x range below 1.0.0.
                upper.major = bumpForUpper(b.major);
                upper.minor = 0;
                upper.patch = 0;
            } else if (c.components == 2) {
                // ^0.0 — bump the minor (Cargo: >=0.0.0, <0.1.0).
                upper.minor = bumpForUpper(b.minor);
                upper.patch = 0;
            } else {
                // ^0.0.x — only the patch may move.
                upper.patch = bumpForUpper(b.patch);
            }
            upper.prerelease.clear();
            if (!(v < upper)) return false;
            break;
        }
        case Clause::Op::Tilde: {
            // ~1.2.3 := >=1.2.3, <1.3.0 ; ~1.2 := >=1.2.0, <1.3.0 ;
            // ~1 := >=1.0.0, <2.0.0 . Major-only (`~1`) bumps the MAJOR;
            // a given minor (`~1.0`, `~1.2`, `~1.2.3`) bumps the minor.
            // `c.components` distinguishes the two after padding.
            if (v < b) return false;
            SemVer upper = b;
            if (c.components <= 1) {
                upper.major = bumpForUpper(b.major);
                upper.minor = 0;
                upper.patch = 0;
            } else {
                upper.minor = bumpForUpper(b.minor);
                upper.patch = 0;
            }
            upper.prerelease.clear();
            if (!(v < upper)) return false;
            break;
        }
        }
    }
    return true;
}

} // namespace tpm
