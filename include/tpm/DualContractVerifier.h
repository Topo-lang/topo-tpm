#ifndef TPM_DUALCONTRACTVERIFIER_H
#define TPM_DUALCONTRACTVERIFIER_H

#include "topo/Sema/SymbolTable.h"

#include <string>
#include <utility>
#include <vector>

// Dual-contract verifier — the migration-correctness gate for declaration
// migration.
//
// The verification proposition:
//   "post-migration conformance to the NEW contract  ≡
//    pre-migration conformance to the OLD contract".
//
// topo-core's `SemanticVerifier` checks "same program, before == after"
// and *requires the two sides to carry the same name set*. The migration
// scenario has differing names across the version boundary, so that
// verifier cannot be used directly. This class is its dual-contract
// extension: it compares the consumer's `.topo` before and after migration
// under the match-key / name-bridge renaming the migration rule supplies.
//
//   L1 — structural layer (MVP, delivered here): the four-domain
//        comparison of `SemanticVerifier`, but with names mapped through
//        a rename map so a bridged rename is not flagged as a missing /
//        unexpected symbol.
//   L2 — stage-topology / visibility layer: NOT delivered in this MVP.
//        See `l2Available()` — every L2 query reports "unavailable",
//        which downgrades the affected call site to a manual warning
//        (never auto). This is a recorded, non-silent gap.

namespace tpm {

/// L1 structural verification result.
struct ContractVerification {
    bool l1Pass = true;                       // structural layer passed
    std::vector<std::string> l1Differences;   // human-readable diffs when not
};

class DualContractVerifier {
public:
    /// L1 — structural verification of the consumer's `.topo` before vs.
    /// after migration. `renameMap` maps an OLD-side symbol name to its
    /// NEW-side name (built from the migration rules' name bridges and
    /// declaration-rename intent); a symbol absent from the map is compared
    /// by identity. Returns L1 pass/fail with diffs.
    ContractVerification verifyL1(
        const topo::SymbolTable& before, const topo::SymbolTable& after,
        const std::vector<std::pair<std::string, std::string>>& renameMap)
        const;

    /// L2 — stage-topology / visibility verification.
    ///
    /// NOT implemented in this MVP. Always returns false. A call site
    /// that passes L1 but cannot clear L2 is downgraded to a manual
    /// warning — so a missing L2 is a conservative, safe gap: it never
    /// causes an unsafe auto migration, only forces more call sites to
    /// manual review.
    static bool l2Available() { return false; }

    /// Human-readable reason recorded against a call site that is held
    /// back because L2 is unavailable.
    static const char* l2UnavailableReason() {
        return "L2 stage-topology/visibility verification is not "
               "implemented in this MVP; the call site is downgraded to "
               "a manual warning";
    }
};

} // namespace tpm

#endif // TPM_DUALCONTRACTVERIFIER_H
