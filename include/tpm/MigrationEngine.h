#ifndef TPM_MIGRATIONENGINE_H
#define TPM_MIGRATIONENGINE_H

#include "tpm/MigrationRule.h"

#include <string>
#include <vector>

// Migration engine — applies declaration-level migration rules to a
// consumer's `.topo`, gated by the dual-contract verifier.
//
// Flow per .topo file:
//   1. Lex the consumer `.topo`.
//   2. For each rule, identify the targeted declaration in the token
//      stream (handler record / operation-fn call site / pipeline-flow
//      edge) — the `.topo`-side "recognise" step.
//   3. Apply the declaration-level transform, landing it as concrete
//      token edits (TokenStreamRewriter is the landing mechanism).
//   4. Run the dual-contract verifier L1 over before/after SymbolTables.
//   5. A call site that clears L1 and (would clear) L2 is an `auto`
//      outcome; anything ambiguous, source-missing, or L2-uncleared is a
//      `manual` warning. Auto edits are committed to the output text;
//      manual sites leave the text untouched.

namespace tpm {

/// One processed call site of the migration report.
struct MigrationReportEntry {
    std::string location;     // "<file>:<line>:<col>"
    std::string declaration;  // qualified name of the migrated declaration
    enum class Outcome { Auto, Manual };
    Outcome outcome = Outcome::Auto;
    std::string rule;         // matched rule id ("kind:target")
    std::string reason;       // precise reason when outcome == Manual
};

inline const char* outcomeName(MigrationReportEntry::Outcome o) {
    return o == MigrationReportEntry::Outcome::Auto ? "auto" : "manual";
}

/// Result of migrating one `.topo` file.
struct MigrationResult {
    bool ok = false;                            // engine ran without a hard error
    std::string error;                          // set when ok == false
    std::string rewrittenSource;                // the migrated `.topo` text
    bool changed = false;                       // rewrittenSource differs from input
    std::vector<MigrationReportEntry> report;   // one entry per processed site

    /// True when every report entry is `auto` (the exit-code-0 case for
    /// `tpm migrate`).
    bool allAuto() const;
    /// Count of `manual`-outcome entries.
    size_t manualCount() const;
};

class MigrationEngine {
public:
    /// Apply one version-to-version rule set to one consumer `.topo` file.
    /// `sourcePath` is used only for diagnostics; `source` is the file
    /// contents.
    MigrationResult migrateSource(const std::string& sourcePath,
                                  const std::string& source,
                                  const MigrationRuleSet& rules) const;
};

} // namespace tpm

#endif // TPM_MIGRATIONENGINE_H
