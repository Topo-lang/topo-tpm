#ifndef TPM_COMMANDS_H
#define TPM_COMMANDS_H

#include <string>
#include <vector>

namespace tpm {

/// CLI sub-command implementations (package-format spec §4). Each returns a
/// process exit code (0 == success).
///
/// `args` excludes the program name and the sub-command verb.

int cmdInit(const std::vector<std::string>& args);
int cmdAdd(const std::vector<std::string>& args);
int cmdRemove(const std::vector<std::string>& args);
int cmdInstall(const std::vector<std::string>& args);
int cmdVerify(const std::vector<std::string>& args);
int cmdTree(const std::vector<std::string>& args);
int cmdPublish(const std::vector<std::string>& args);
int cmdMigrate(const std::vector<std::string>& args);

/// Print the top-level usage text.
void printUsage();

} // namespace tpm

#endif // TPM_COMMANDS_H
