# Third-party components

`topo-tpm` vendors two header-only libraries under `third_party/` and
depends on one third-party library resolved at configure time.

## Vendored (under `third_party/`)

### picosha2 — `third_party/picosha2/`

A header-only SHA-256 implementation, used by `lib/Sha256.cpp` for
content addressing in the package cache.

- Upstream: https://github.com/okdshin/PicoSHA2
- License: MIT — see `third_party/picosha2/LICENSE`
- Vendored copy: a single `picosha2.h` plus the upstream LICENSE.

### semver — `third_party/semver/`

A header-only SemVer parser, used by `lib/SemVer.cpp` for version range
satisfaction in manifest / lockfile resolution.

- Upstream: https://github.com/Neargye/semver
- License: MIT — see `third_party/semver/LICENSE`
- Vendored copy: a single `semver.hpp` plus the upstream LICENSE.

## Resolved at configure time (not vendored)

### tomlplusplus — `find_path(TOMLPLUSPLUS_INCLUDE_DIR ...)`

Header-only TOML parser, used everywhere we touch manifests, lockfiles,
or migration rules.

- Upstream: https://github.com/marzer/tomlplusplus
- License: MIT (upstream `LICENSE`)
- Resolution: declared in `vcpkg.json` so a vcpkg-toolchain configure
  pulls a known-good version; a system install (`brew install
  tomlplusplus`, distribution package, …) also works because we resolve
  via `find_path()`.

### GoogleTest — `FetchContent_Declare`

Pinned to `v1.15.2`. Only fetched when `TOPO_TPM_BUILD_TESTS=ON` and the
project is the top-level build (`PROJECT_IS_TOP_LEVEL`). Consumers that
take `topo-tpm` as a subproject and turn the option off do not pay the
network cost.

- Upstream: https://github.com/google/googletest
- License: BSD-3-Clause
