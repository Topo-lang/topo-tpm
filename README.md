# topo-tpm

The Topo package manager — installs, locks, migrates, and publishes
`.topo`-declaration packages. One of the independent repositories that
together make up the [Topo](https://github.com/topo-lang/topo-core)
toolchain.

## What it does

A `.topo`-package consumer writes declarations in `Topo.toml` plus a
manifest pointing at dependency packages. `tpm` resolves that manifest
into a deterministic install set (`tpm.lock`), fetches the source
through a git-backed registry, content-addresses the package, and
applies declaration migrations when a dependency's `.topo` contract
evolves across major versions.

The migration mechanism reuses the topo-core frontend (Lexer / Parser /
Sema → SymbolTable) and extends topo-core's `SemanticVerifier` for the
dual-contract check — this is the only place `topo-tpm` touches the
host-language compiler frontend; the rest of the package-management
core has zero topo-core *frontend* compile dependency.

## Building from source

`topo-tpm` builds and tests standalone — the only Topo dependency it
needs is `topo-core` resolved through CMake `find_package`.

```sh
# 1. Build & install topo-core to a prefix (until packaged releases land):
cmake -S /path/to/topo-core -B /tmp/topo-core/build \
    -DCMAKE_INSTALL_PREFIX="$HOME/topo-install"
cmake --build /tmp/topo-core/build --target install

# 2. Configure & build topo-tpm against that prefix:
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="$HOME/topo-install"
cmake --build build

# 3. Run tests:
ctest --test-dir build --output-on-failure
```

A vcpkg toolchain works too — the `tomlplusplus` dependency is
declared in `vcpkg.json`:

```sh
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_PREFIX_PATH="$HOME/topo-install"
```

## Using `topo-tpm` from another CMake project

After installing topo-tpm to a prefix, downstream projects do:

```cmake
find_package(topo-tpm CONFIG REQUIRED)
target_link_libraries(<your-target> PRIVATE topo::tpm::TpmLib)
```

`topo::tpm::TpmLib` transitively pulls the topo-core frontend targets
it depends on (Platform / Parser / Sema / Transform), so the consumer
does not need to add `topo-core` to its own `target_link_libraries`.

## Project status

`topo-tpm` is the MVP of the package-management mechanism described in
the [declaration-package spec](https://github.com/topo-lang/topo-core)
(see `.aidesk/base/60-spec/topo-tpm/` in the topo-core repo). `install`,
`migrate`, and `publish` work end-to-end with a git-backed registry;
the migration engine handles the three-path resolution
(builtin → topo-app → tpm-package, priority `tpm > topo-app >
builtin`) and the dual-contract verifier extends topo-core's
`SemanticVerifier`.

## Repository layout

```
include/tpm/     Public headers (Manifest, Lock, SemVer, MigrationEngine, …)
lib/             Implementation
tools/tpm/       The `tpm` CLI executable
test/unit/       GoogleTest unit suites (engine, rule parser, verifier, …)
test/cli/        Subprocess CLI integration tests (install / migrate / publish)
third_party/     Vendored header-only deps (picosha2, semver) — see THIRD-PARTY.md
cmake/           TpmCompilerFlags + Config.cmake.in
```

## License

MIT — see `LICENSE`. The vendored third-party headers carry their own
licenses under `third_party/<lib>/LICENSE`; see `THIRD-PARTY.md`.

This project is developed with substantial AI assistance — see
`AI-DECLARATION.md`.
