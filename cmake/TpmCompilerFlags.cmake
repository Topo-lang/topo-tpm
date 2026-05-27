# TpmCompilerFlags.cmake — standalone compiler-flag helper for topo-tpm.
#
# Carved out of the monorepo's cmake/TopoCompilerFlags.cmake so this repo
# does not depend on monorepo helpers. The behavior here matches the
# subset that topo-tpm used in the monorepo build (cxx_std_17, warnings,
# optional sanitizer); LLVM-flag and PCH-reuse helpers are intentionally
# omitted because topo-tpm has no LLVM-linking targets and is small
# enough that PCH is not worth the wiring.

# RPATH configuration for Unix shared library builds
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

# ── Sanitizer support ─────────────────────────────────
# Usage: cmake -B build -DTOPO_TPM_SANITIZER=address
#        cmake -B build -DTOPO_TPM_SANITIZER=undefined
#        cmake -B build -DTOPO_TPM_SANITIZER=address,undefined
set(TOPO_TPM_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

if(TOPO_TPM_SANITIZER)
    message(STATUS "topo-tpm sanitizers enabled: ${TOPO_TPM_SANITIZER}")
endif()

function(tpm_apply_sanitizer target)
    if(NOT TOPO_TPM_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_TPM_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_TPM_SANITIZER})
    endif()
endfunction()

function(tpm_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()

    tpm_apply_sanitizer(${target})
endfunction()
