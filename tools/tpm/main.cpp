// tpm — the Topo package manager (MVP).
//
// Distributes Topo model artifacts (.topo declarations, Functor layout
// contracts, event-protocol schemas, stdlib type definitions, declaration
// kernels) that host package managers cannot carry.
//
// Zero topo-core *compile* dependency for the package-management core:
// tpm lives in its own top-level directory and only link-depends on
// TopoPlatform for the cross-platform subprocess primitive used to drive
// `git`. The declaration-migration engine additionally reuses the
// topo-core frontend (Lexer/Parser/Sema -> SymbolTable) because migration
// is a declaration-level transform and can only be verified against
// parsed declarations.

#include "tpm/Commands.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        tpm::printUsage();
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i)
        args.emplace_back(argv[i]);

    if (command == "-h" || command == "--help" || command == "help") {
        tpm::printUsage();
        return 0;
    }
    if (command == "init") return tpm::cmdInit(args);
    if (command == "add") return tpm::cmdAdd(args);
    if (command == "remove") return tpm::cmdRemove(args);
    if (command == "install") return tpm::cmdInstall(args);
    if (command == "verify") return tpm::cmdVerify(args);
    if (command == "tree") return tpm::cmdTree(args);
    if (command == "publish") return tpm::cmdPublish(args);
    if (command == "migrate") return tpm::cmdMigrate(args);

    std::cerr << "error: unknown command '" << command << "'\n\n";
    tpm::printUsage();
    return 1;
}
