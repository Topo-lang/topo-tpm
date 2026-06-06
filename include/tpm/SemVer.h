#ifndef TPM_SEMVER_H
#define TPM_SEMVER_H

#include <optional>
#include <string>
#include <vector>

namespace tpm {

/// A parsed SemVer 2.0.0 version (major.minor.patch, pre-release ignored for
/// ordering beyond a simple comparison — sufficient for MVP tag selection).
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease; // empty when none

    /// Parse "1.2.3" / "v1.2.3" / "1.2.3-rc1". Returns nullopt on malformed input.
    static std::optional<SemVer> parse(const std::string& text);

    std::string toString() const;

    /// Total order: numeric fields first, then a release > pre-release rule.
    int compare(const SemVer& other) const;
    bool operator<(const SemVer& o) const { return compare(o) < 0; }
    bool operator==(const SemVer& o) const { return compare(o) == 0; }
};

/// A version requirement following Cargo conventions: `^1.2`, `~1.2.3`,
/// `>=1.0.0`, `<2.0.0`, exact `1.2.3`, `*`, and comma-separated ranges
/// (`>=4.0.0, <5.0.0`).
class VersionReq {
public:
    /// Parse a requirement string. Returns nullopt on malformed input.
    static std::optional<VersionReq> parse(const std::string& text);

    /// True when `v` satisfies every clause of the requirement.
    bool matches(const SemVer& v) const;

    const std::string& raw() const { return raw_; }

private:
    struct Clause {
        enum class Op { Caret, Tilde, Gte, Gt, Lte, Lt, Eq, Any } op = Op::Any;
        SemVer bound;
        // How many `major.minor.patch` components the user actually wrote
        // (1, 2, or 3) before `splitVersion` padded the short form. The
        // caret/tilde upper-bound math needs this to tell `~1` (major-only,
        // `<2.0.0`) from `~1.0` (minor given, `<1.1.0`), and `^0`
        // (major-only, `<1.0.0`) from `^0.0.0` (`<0.0.1`). 3 when unknown.
        int components = 3;
    };
    std::vector<Clause> clauses_;
    std::string raw_;
};

} // namespace tpm

#endif // TPM_SEMVER_H
