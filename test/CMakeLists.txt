# topo-tpm unit tests — declaration-migration mechanism (MVP).
#
# Covers migration-rule parsing (index.toml + *.migration.toml), the
# migration engine's three-path handling, and the dual-contract verifier.
# Zero LLVM dependency.

# MinGW on GHA: cached upstream topo-install may have been linked against
# a different libstdc++/libgcc ABI than C:\mingw64\bin\ currently provides,
# tripping STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139) when
# gtest_discover_tests launches each binary. Static-link the C++ runtime
# for every test exe in this directory so they're hermetic.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

find_path(TOMLPLUSPLUS_INCLUDE_DIR_TPM_TESTS toml++/toml.hpp)

add_executable(tpm-tests
    unit/MigrationRuleTest.cpp
    unit/MigrationEngineTest.cpp
    unit/MigrateOrchestrationTest.cpp
    unit/DualContractVerifierTest.cpp
    unit/PathTraversalRegressionTest.cpp
    unit/FileLockConcurrencyTest.cpp
    unit/CommandsTest.cpp
    unit/SemVerTest.cpp
)
target_link_libraries(tpm-tests PRIVATE
    TpmLib
    topo::core::TopoPlatform
    GTest::gtest_main
)
target_include_directories(tpm-tests PRIVATE
    ${TOMLPLUSPLUS_INCLUDE_DIR_TPM_TESTS}
)
target_compile_definitions(tpm-tests PRIVATE
    TPM_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
tpm_set_compiler_flags(tpm-tests)
gtest_discover_tests(tpm-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;topo-tpm" TIMEOUT 60)

# tpm CLI integration tests — spawn the built `tpm` binary as a
# subprocess against a scratch project root. Covers `install --from`,
# `migrate` rejection on missing tpm.lock, and `publish` stub exit.
#
# tpm-cli integration tests spawn the built tpm via
# topo::platform::runProcessCapture. Previously Windows-skipped: the spawn hung
# on the GHA windows-2022 runner (tpm.exe never returned, while ubuntu/macOS
# finished each test in ~10 ms). Root cause + fix in topo-core — runProcessCapture
# now bounds reproc's drain with a process deadline, so the Windows
# loopback-socket EOF that failed to arrive can no longer hang the call. Runs on
# all three platforms now.
add_executable(tpm-cli-integration-tests
    cli/TpmCliIntegrationTest.cpp
)
target_link_libraries(tpm-cli-integration-tests PRIVATE
    topo::core::TopoPlatform
    GTest::gtest_main
)
target_compile_definitions(tpm-cli-integration-tests PRIVATE
    TOPO_TPM_BINARY="$<TARGET_FILE:tpm>"
    TPM_CLI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cli/fixtures"
)
add_dependencies(tpm-cli-integration-tests tpm)
tpm_set_compiler_flags(tpm-cli-integration-tests)
gtest_discover_tests(tpm-cli-integration-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;topo-tpm\;realpath" TIMEOUT 60)
