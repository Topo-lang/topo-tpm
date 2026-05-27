cmake_minimum_required(VERSION 3.20)

project(topo-tpm
    VERSION 0.1.0
    DESCRIPTION "Topo package manager — declaration-package install / migrate / publish"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-tpm"
    LANGUAGES CXX
)

# topo-tpm — Topo package manager (MVP).
#
# An independent top-level toolchain. The package-management core
# (manifest, lock, registry, cache) has ZERO topo-core *frontend* compile
# dependency — it only link-depends on topo::core::TopoPlatform for the
# cross-platform subprocess primitive that drives `git`.
#
# The declaration-migration mechanism (declaration-migration spec) is the
# one part that, by design, reuses the topo-core frontend: the engine
# parses the consumer's `.topo` (Lexer / Parser / Sema -> SymbolTable) and
# the dual-contract verifier extends topo-core's SemanticVerifier. The
# spec (§4.1 / §5 / §9) explicitly sanctions this coupling — migration is
# a declaration-level transform and can only be verified against parsed
# declarations. The link below is therefore spec-driven, not incidental.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TpmCompilerFlags)

# ── Upstream Topo packages ────────────────────────────
# topo-core exports Platform / Parser / Sema / Transform under the
# `topo::core::` namespace via its TopoCoreTargets export set.
# Guarded so a meta-build that already populated topo-core targets (e.g.
# via add_subdirectory) does not redundantly find_package.
if(NOT TARGET topo::core::TopoPlatform)
    find_package(topo-core CONFIG REQUIRED)
endif()

# ── Third-party dependencies ──────────────────────────
# tomlplusplus — header-only TOML parser (manifests, lockfiles, migration
# rules). Resolved from vcpkg.json when CMAKE_TOOLCHAIN_FILE points at
# vcpkg.cmake, or from a system install otherwise.
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

# picosha2 / semver are vendored under third_party/ as header-only INTERFACE
# libraries — see THIRD-PARTY.md for license attribution.
add_library(tpm_picosha2 INTERFACE)
add_library(topo::tpm::tpm_picosha2 ALIAS tpm_picosha2)
target_include_directories(tpm_picosha2 INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/picosha2>
)

add_library(tpm_semver INTERFACE)
add_library(topo::tpm::tpm_semver ALIAS tpm_semver)
target_include_directories(tpm_semver INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/semver>
)

# ── tpm support library ───────────────────────────────
add_library(TpmLib STATIC
    lib/Sha256.cpp
    lib/SemVer.cpp
    lib/Manifest.cpp
    lib/Lock.cpp
    lib/PackageHash.cpp
    lib/GitRegistry.cpp
    lib/Cache.cpp
    lib/MigrationRule.cpp
    lib/MigrationEngine.cpp
    lib/DualContractVerifier.cpp
    lib/Commands.cpp
)
add_library(topo::tpm::TpmLib ALIAS TpmLib)

target_include_directories(TpmLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(TpmLib PRIVATE
    ${TOMLPLUSPLUS_INCLUDE_DIR}
)

# topo::core::TopoPlatform PUBLIC-propagates <topo/Platform/Process.h> for
# the git subprocess primitive. TopoParser / TopoSema / TopoTransform give
# the migration engine the frontend it parses the consumer `.topo` with and
# the SemanticVerifier the dual-contract verifier extends — see the file
# header for why this coupling is spec-sanctioned.
target_link_libraries(TpmLib
    PUBLIC
        topo::core::TopoPlatform
        topo::core::TopoParser
        topo::core::TopoSema
        topo::core::TopoTransform
    PRIVATE
        tpm_picosha2
        tpm_semver
)

tpm_set_compiler_flags(TpmLib)

# ── tpm executable ────────────────────────────────────
add_executable(tpm
    tools/tpm/main.cpp
)
target_link_libraries(tpm PRIVATE TpmLib)
tpm_set_compiler_flags(tpm)

# ── Tests ─────────────────────────────────────────────
# `option()` so consumers building topo-tpm as a dependency
# (e.g. downstream packages doing `add_subdirectory`) can opt out.
option(TOPO_TPM_BUILD_TESTS "Build topo-tpm tests" ON)
if(TOPO_TPM_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    # GoogleTest is fetched lazily so the tree builds on a network-less
    # consumer that turns -DTOPO_TPM_BUILD_TESTS=OFF.
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
    add_subdirectory(test)
endif()

# ── Install / export ──────────────────────────────────
install(TARGETS TpmLib tpm tpm_picosha2 tpm_semver
    EXPORT topo-tpmTargets
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/tpm
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT topo-tpmTargets
    FILE topo-tpmTargets.cmake
    NAMESPACE topo::tpm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-tpm
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-tpmConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-tpmConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-tpm
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-tpmConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-tpmConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-tpmConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-tpm
)
