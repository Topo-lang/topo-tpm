#include "tpm/DualContractVerifier.h"

#include "topo/AST/ASTNode.h"

#include <map>
#include <set>

namespace tpm {
namespace {

/// Apply the OLD→NEW rename map to a name. A name absent from the map maps
/// to itself.
std::string mapName(const std::map<std::string, std::string>& rm,
                     const std::string& name) {
    auto it = rm.find(name);
    return it == rm.end() ? name : it->second;
}

/// A *structural* signature of a type that is stable across a migration's
/// intended record reshape.
///
/// L1 verifies that migration did not add / remove / change-the-*shape*-of
/// a declaration — the L1 invariant is "migration must not add, remove, or
/// reshape a declaration". A handler-In `record<...>` field add / remove /
/// retype is exactly the change a migration rule is *meant* to make —
/// comparing the record's field list verbatim would reject every legitimate
/// handler-path migration.
///
/// So L1 normalizes a `record<...>` to `record<>` (its structural kind,
/// not its field list) and a `union<...>` likewise. Field-level
/// correctness inside the record is owned by the match-key logic in the
/// engine, not re-litigated here. A genuine shape change — record turning
/// into a scalar, a different stdlib type, a changed pointer / ownership
/// modifier — still differs and is still caught.
std::string structuralSignature(const topo::TypeNode& t) {
    std::string sig;
    if (t.isConst) sig += "const ";
    sig += std::string(topo::ownershipKindName(t.ownership)) + " ";
    if (!t.recordFields.empty()) {
        // record<...> / union<...> — normalize the field list away.
        for (size_t i = 0; i < t.nameParts.size(); ++i) {
            if (i) sig += "::";
            sig += t.nameParts[i];
        }
        sig += "<...>";
    } else {
        sig += t.toString();
    }
    if (t.modifier == topo::TypeNode::Ref) sig += "&";
    else if (t.modifier == topo::TypeNode::Ptr) sig += "*";
    return sig;
}

} // namespace

ContractVerification DualContractVerifier::verifyL1(
    const topo::SymbolTable& before, const topo::SymbolTable& after,
    const std::vector<std::pair<std::string, std::string>>& renameMap) const {

    ContractVerification result;

    std::map<std::string, std::string> rm(renameMap.begin(), renameMap.end());

    // The migration verifier replays the four-domain comparison of
    // topo-core's `SemanticVerifier` (functions / classes / logic blocks /
    // constraints — the L1 structural layer) but, instead of requiring
    // "same name set on both sides", projects the BEFORE side's names
    // through the rename map and then compares. A bridged rename therefore
    // matches; a genuine add / remove / shape change still surfaces as a
    // diff.

    // ── functions: count, presence, param count, return type ────────────
    {
        const auto& a = before.functions();
        const auto& b = after.functions();
        if (a.size() != b.size())
            result.l1Differences.push_back(
                "function count changed: " + std::to_string(a.size()) +
                " -> " + std::to_string(b.size()));

        std::set<std::string> bNames;
        for (const auto& [name, _] : b) bNames.insert(name);

        for (const auto& [name, fa] : a) {
            std::string mapped = mapName(rm, name);
            auto itB = b.find(mapped);
            if (itB == b.end()) {
                result.l1Differences.push_back(
                    "function '" + name + "' (-> '" + mapped +
                    "') has no counterpart after migration");
                continue;
            }
            bNames.erase(mapped);
            const auto& fb = itB->second;
            if (fa.params.size() != fb.params.size())
                result.l1Differences.push_back(
                    "function '" + mapped + "' parameter count changed: " +
                    std::to_string(fa.params.size()) + " -> " +
                    std::to_string(fb.params.size()));
            // Return type compared by structural signature: a record-field
            // reshape (the migration's intended delta) is normalized away;
            // a genuine type-shape change is still caught.
            if (structuralSignature(fa.returnType) !=
                structuralSignature(fb.returnType))
                result.l1Differences.push_back(
                    "function '" + mapped + "' return type shape changed: '" +
                    structuralSignature(fa.returnType) + "' -> '" +
                    structuralSignature(fb.returnType) + "'");
            // Parameter types compared the same way, slot by slot.
            for (size_t pi = 0;
                 pi < fa.params.size() && pi < fb.params.size(); ++pi) {
                if (structuralSignature(fa.params[pi].type) !=
                    structuralSignature(fb.params[pi].type))
                    result.l1Differences.push_back(
                        "function '" + mapped + "' parameter " +
                        std::to_string(pi) + " type shape changed: '" +
                        structuralSignature(fa.params[pi].type) + "' -> '" +
                        structuralSignature(fb.params[pi].type) + "'");
            }
            // Positional comparison above does not catch a parameter reorder
            // when both sides carry the same multiset of (name, type) pairs
            // but at different positions (e.g. fn(x:int, y:int) -> fn(y:int,
            // x:int) — types match slot-by-slot but the caller-visible
            // parameter order has flipped, which is a breaking change for
            // every positional call site). Flag this as a "potential
            // reorder" L1 diagnostic. The `param_changes` migration rule
            // is not yet wired, so this is a forward-looking guard against
            // silent breakage rather than a gate on an existing rewrite
            // path.
            if (fa.params.size() == fb.params.size() &&
                fa.params.size() > 1) {
                bool anyPositionalNameMismatch = false;
                for (size_t pi = 0; pi < fa.params.size(); ++pi) {
                    if (fa.params[pi].name != fb.params[pi].name) {
                        anyPositionalNameMismatch = true;
                        break;
                    }
                }
                if (anyPositionalNameMismatch) {
                    auto sigPair = [](const topo::Parameter& p) {
                        return p.name + ":" + structuralSignature(p.type);
                    };
                    std::multiset<std::string> aPairs;
                    std::multiset<std::string> bPairs;
                    for (const auto& p : fa.params) aPairs.insert(sigPair(p));
                    for (const auto& p : fb.params) bPairs.insert(sigPair(p));
                    if (aPairs == bPairs) {
                        result.l1Differences.push_back(
                            "function '" + mapped +
                            "' parameter order changed (same (name, type) "
                            "multiset, different positions) — potential "
                            "reorder, breaks positional call sites");
                    }
                }
            }
        }
        for (const auto& leftover : bNames)
            result.l1Differences.push_back(
                "function '" + leftover +
                "' appeared after migration with no pre-migration origin");
    }

    // ── classes: count, presence, member-function count ─────────────────
    {
        const auto& a = before.classSymbols();
        const auto& b = after.classSymbols();
        if (a.size() != b.size())
            result.l1Differences.push_back(
                "class count changed: " + std::to_string(a.size()) + " -> " +
                std::to_string(b.size()));
        std::set<std::string> bNames;
        for (const auto& [name, _] : b) bNames.insert(name);
        for (const auto& [name, ca] : a) {
            std::string mapped = mapName(rm, name);
            auto itB = b.find(mapped);
            if (itB == b.end()) {
                result.l1Differences.push_back(
                    "class '" + name + "' (-> '" + mapped +
                    "') has no counterpart after migration");
                continue;
            }
            bNames.erase(mapped);
            if (ca.memberFunctions.size() != itB->second.memberFunctions.size())
                result.l1Differences.push_back(
                    "class '" + mapped + "' member-function count changed: " +
                    std::to_string(ca.memberFunctions.size()) + " -> " +
                    std::to_string(itB->second.memberFunctions.size()));
        }
        for (const auto& leftover : bNames)
            result.l1Differences.push_back(
                "class '" + leftover +
                "' appeared after migration with no pre-migration origin");
    }

    // ── logic blocks: count, presence, isPipeline flag, edge count ──────
    {
        const auto& a = before.logicBlocks();
        const auto& b = after.logicBlocks();
        if (a.size() != b.size())
            result.l1Differences.push_back(
                "logic-block count changed: " + std::to_string(a.size()) +
                " -> " + std::to_string(b.size()));
        std::set<std::string> bNames;
        for (const auto& [name, _] : b) bNames.insert(name);
        for (const auto& [name, la] : a) {
            std::string mapped = mapName(rm, name);
            auto itB = b.find(mapped);
            if (itB == b.end()) {
                result.l1Differences.push_back(
                    "logic block '" + name + "' (-> '" + mapped +
                    "') has no counterpart after migration");
                continue;
            }
            bNames.erase(mapped);
            const auto& lb = itB->second;
            if (la.isPipeline != lb.isPipeline)
                result.l1Differences.push_back(
                    "logic block '" + mapped + "' pipeline flag changed");
            if (la.edges.size() != lb.edges.size())
                result.l1Differences.push_back(
                    "logic block '" + mapped + "' edge count changed: " +
                    std::to_string(la.edges.size()) + " -> " +
                    std::to_string(lb.edges.size()));
        }
        for (const auto& leftover : bNames)
            result.l1Differences.push_back(
                "logic block '" + leftover +
                "' appeared after migration with no pre-migration origin");
    }

    // ── constraints: count, presence, member count ──────────────────────
    {
        const auto& a = before.constraintSymbols();
        const auto& b = after.constraintSymbols();
        if (a.size() != b.size())
            result.l1Differences.push_back(
                "constraint count changed: " + std::to_string(a.size()) +
                " -> " + std::to_string(b.size()));
        std::set<std::string> bNames;
        for (const auto& [name, _] : b) bNames.insert(name);
        for (const auto& [name, ca] : a) {
            std::string mapped = mapName(rm, name);
            auto itB = b.find(mapped);
            if (itB == b.end()) {
                result.l1Differences.push_back(
                    "constraint '" + name + "' (-> '" + mapped +
                    "') has no counterpart after migration");
                continue;
            }
            bNames.erase(mapped);
            if (ca.members.size() != itB->second.members.size())
                result.l1Differences.push_back(
                    "constraint '" + mapped + "' member count changed: " +
                    std::to_string(ca.members.size()) + " -> " +
                    std::to_string(itB->second.members.size()));
        }
        for (const auto& leftover : bNames)
            result.l1Differences.push_back(
                "constraint '" + leftover +
                "' appeared after migration with no pre-migration origin");
    }

    result.l1Pass = result.l1Differences.empty();
    return result;
}

} // namespace tpm
