# bc-file-system

A C23 file-system core with one header and C++, Swift and Rust faces.

Two house rules, inject-never-reach and no-default-arguments, need a file system that code is *handed* rather
than one it reaches for. This library is that file system, written once as a C core so that C++, Swift and
Rust consumers share one engine, one error vocabulary and one contract suite. It earns its place through what
the standard libraries do not do: kernel-enforced containment (`beneath` a held directory, with every component
opened `O_NOFOLLOW | O_DIRECTORY` against the held parent), refusal to replace, kill-safety of every multi-step
write, and one fault gate every face can use: an observer in the engine that fails a scripted system call or
stops an operation without cleanup, as if the process had been killed there.

The names: the C prefix is `bc_file_system_` and the header `<bc_file_system/bc_file_system.h>`; the C++
namespace is `bc::file_system` with headers `<bc_file_system/….hpp>`; CMake builds `bc_file_system_core` (C)
and `bc_file_system` (C++), linked as `bc::file_system_core` and `bc::file_system`; the Swift products are
`BCFileSystem` and `BCFileSystemTestFixtures` over the C module `CBCFileSystem`; the Rust crates are
`bc-file-system` and `bc-file-system-testing`. Licence: Apache-2.0 (`LICENSE.txt`).

## Consumer-host floor

The published consumer-host floor is GCC 14, clang 19, Apple clang 21 (Xcode 27), NDK r28 or later and
Emscripten 2025 or later: C23 in its GNU dialect with `<stdckdint.h>`. A host below it fails at the floor check
in the core's private platform header with a clear error. The Rust crate `bc-file-system` (`crates/`, Rust 1.89
or later, `publish = false`: pin it by git SHA) compiles the same core with the consumer's own C compiler, so the
floor is its floor too, and its build script names the floor and the compiler it found when a host falls below.

## Building and testing

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset sanitized && cmake --build --preset sanitized && ctest --preset sanitized   # ASan + UBSan
cmake --preset hardened && cmake --build --preset hardened                                 # the production posture
swift test
cargo test --manifest-path crates/Cargo.toml --locked
bash scripts/verify.sh            # every leg: both builds, the header four ways, the guards, the consumer, swift test, cargo
```

Build output goes under `build/<preset>/`. GoogleTest is fetched by CMake, pinned to a commit, and only when this
repository is the top-level project; a consumer that pulls the library in through `FetchContent` (see
`examples/consumer/`) gets the two libraries and nothing else. The standards the repository holds itself to,
and how its guards enforce them, are in `AGENTS.md`.

## Where it stands

Stage 9 of its plan, `salvaged-under-the-new-name`: the public header, an honest stub core that answers
`unsupported` to every operation, the C++ face, the contract suite (red or disabled until each operation's stage),
the binding suites, the Swift skeleton, the guards and CI. Stage 10 `path-module` is in: the path module (`core/src/path.c`), the freestanding tier of the core, compiled
on every build under `-ffreestanding` and for bare wasm32, proven by a C-only test and fuzzed under the `fuzz`
preset. Stage 11 `engine-and-errors` is in: the engine probes the platform's fast paths once at creation, the
observer's mechanics are real (every step counted, a scripted fault firing at its occurrence, a stop as a
verdict), the platform layer has its durable sync and no-replace rename, and the Rust crate's ABI smoke proves
the hand-written declarations against the core. Stage 12 `resolution-layer` is in: every location resolves once
to a parent descriptor and a leaf name with containment enforced by the kernel (the per-component walk with the
`st_dev`/`st_ino` identity check, the `openat2` and `O_NOFOLLOW_ANY` fast paths and the walk they fall back to),
`open_directory` is the first real operation, and the C-only resolve test holds the swapped-directory race
deterministically. Stage 13 `status-and-reads` is in: `status` (absence is an answer, `lstat` semantics under
`FinalSymlink`) and `read_file` (the leaf opened and judged by its descriptor, the read limit, the buffer
protocol, a file that grew between its measurement and its reading asked for again) are real on every face, each
system call behind the observer and asked again on EINTR. Stage 14 `writes-in-c` is in: `write_file` (every write
staged in an exclusive temporary beside its destination and renamed into place, `replace` or `refuse`, a final
symlink refused or followed to the file it names, the mode set exactly, `durable` syncing the file and its
directory), `append` and `sync` are real on every face; "stop without cleanup" is real, so the kill-point cases
hold the destination whole or replaced at every step of a write; and every gated system call lives in one unit,
`core/src/calls.c`. Stage 15 `directories-in-c` is in: `create_directory` and `create_directories` (made with a
private mode and given the stated one exactly, the umask never consulted), `list_directory` (sorted; counted
before stored, so an array too small stores nothing), `remove_file`, `remove_directory`, `remove_tree` (by
`unlinkat` on held descriptors with a window of sixteen levels and no recursion, the root of the file system
refused by spelling and by identity, a mount point inside the tree refused as `cross_device`),
`create_temporary_directory` and `set_permissions` are real on every face; and the walk opens every component
for search only (`O_SEARCH` on Apple platforms, `O_PATH` on Linux), so a directory the process may enter but
not read is a parent like any other. Stage 16 `moves-and-copies-in-c` is in: `rename` (one step, `renameat` under
`replace` and the platform's no-replace rename under `refuse`, both parents held, `cross_device` never a copy and a
delete, a durable rename syncing both directories) and `copy_file` (staged like a write and atomic at the
destination; the bytes by the platform's clone where it has one, the kernel's `copy_file_range` next, the
read-and-write loop otherwise, with the kill-point table holding the destination whole or replaced at every step)
are real on every face. The remaining operations land in C in the next stage (locks and links); `0.1.0` is tagged
when every face is green on every platform.
