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

## Using the Swift package

Two products: `BCFileSystem` is the library (the engine, the `FileSystem` a piece of code is handed, the
`Location` it acts on, the path values `AbsolutePath` and `RelativePath` with the path module's functions on
`PathText`, and the typed `FileSystemError`); `BCFileSystemTestFixtures` is for a consumer's tests only (a
`TemporaryDirectory` the test owns, and `FileSystem.observed(root:)`, a file system over an engine with the
observer installed, so a test can script a failing system call or a kill point and read step counts, with
`FileSystem.portableOnly(root:)` beside it, the same engine refused every fast path, so a test can run a second
time over the path every platform has). Neither
needs C++ interoperability switched on. Until `1.0.0` any release may change the interface, so pin a commit; the
public URL is decided before `0.1.0`:

```swift
dependencies: [
    .package(url: "<the public URL, decided before 0.1.0>", revision: "<sha>"),
],
targets: [
    .target(name: "Tool", dependencies: [.product(name: "BCFileSystem", package: "bc-file-system")]),
    .testTarget(
        name: "ToolTests",
        dependencies: ["Tool", .product(name: "BCFileSystemTestFixtures", package: "bc-file-system")]
    ),
]
```

The composition root names the live engine once and hands the file system down; a test makes its own, over a
directory it owns:

```swift
let fileSystem = FileSystem(engine: try Engine.live())                     // in main, and nowhere below it
let observed = try FileSystem.observed(root: TemporaryDirectory.create())  // in a test
```

The manifest's tools version is 6.0; the toolchain that builds it must meet the floor above (Swift 6.4, Xcode 27
on Apple platforms), because SwiftPM compiles the core with the toolchain's own clang.

## Using the Rust crates

Two crates: `bc-file-system` is the library (the `Engine`, whose methods are the operations; the `Location` they
act on, an `AbsolutePath` or a `RelativePath` beneath an open `Directory`; the path module's functions in
`bc_file_system::path`; the options every operation states, none with a default; and the typed `Error`);
`bc-file-system-testing` is for a consumer's tests only (`ObservedEngine`, an engine with the observer installed,
so a test can script a failing system call or a kill point and read step counts, and `TemporaryDirectory`, a
directory the test owns). The core's C sources sit outside the crate directory, so the crates are not published
(`publish = false`): pin a commit, from the same repository as the Swift package; the public URL is decided before
`0.1.0`. The crates are experimental until a production consumer runs on them.

```toml
[dependencies]
bc-file-system = { git = "<the public URL, decided before 0.1.0>", rev = "<sha>" }

[dev-dependencies]
bc-file-system-testing = { git = "<the public URL, decided before 0.1.0>", rev = "<sha>" }
```

The composition root names the live engine once and hands `&Engine` down; a test makes an observed one, over a
directory it owns:

```rust
let engine = Engine::live()?;                                // in main, and nowhere below it
let observed = ObservedEngine::observed_for_tests()?;        // in a test
let directory = TemporaryDirectory::create("my-test")?;      // in a test
```

The crates need Rust 1.89 or later, and compile the core with the consumer's own C compiler, so the floor above is
theirs too. The core is POSIX: the crates build for the Unix family only.

## Building and testing

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset sanitized && cmake --build --preset sanitized && ctest --preset sanitized   # ASan + UBSan
cmake --preset hardened && cmake --build --preset hardened                                 # the production posture
swift test
cargo test --manifest-path crates/Cargo.toml --locked
RUSTDOCFLAGS='-D warnings' cargo doc --manifest-path crates/Cargo.toml --no-deps --locked
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
are real on every face. Stage 17 `locks-links-condemn` is in: `try_lock` (the platform's `flock` over an open of
the entry's own, `held_elsewhere` an answer and never a wait, `unsupported` where `flock` is a stub),
`create_symlink` and `read_symlink` (the target passed to the system unchanged and read back whole under the
buffer protocol), `condemn` (one no-replace rename to `.bc-condemned-<name>` beside the entry, a stale leftover
told apart by a token) and `remove_condemned` (the tree removal over every condemned entry and every staged file a
killed write left) are real on every face, so every operation of the header is; the error struct says which end of
a `rename` or `copy_file` a failure belongs to (`failed_location`, 280 to 284 bytes, every face's pin moved); the
ABI-argument and operation-sequence fuzzers run beside the path fuzzer under the `fuzz` preset; and nothing of the
draft facade reached this repository. Stage 18 `swift-package` is in: the Swift path values validate through the path
module's two validators, so the rules have one home and a Swift value refuses what every other face refuses (`/a/`,
`//`, `/.`, `a/./b`, a 256-byte component); the module's ten functions are on the Swift face as `PathText`, with the
C++ path suite's cases ported to Swift; the Swift `Operation` enum covers the header's 39 operations; and the
house-rules guard's face-parity leg reads the Swift face. Stage 19 `rust-crates` is in: the Rust face is one `&self`
method on `Engine` per header operation, over `Location`, `Directory`, `Lock`, the options and the path values, with
the path module's ten functions and the compositions (`read_file_if_present`, `lines`, the bounded grow loop);
`bc-file-system-testing` holds the observed engine and the temporary directory a consumer's tests use; the error is
one pointer wide; `cargo doc` joins the `rust` leg; and bc-mutants' four `fs.rs` tests pass, copied into the
crate's binding suite over compositions of its API. Stage 26 `portable-only-run` is in: a third factory for
tests, `bc_file_system_engine_create_portable_only_for_tests` (on every face beside the observed one), makes the
observed engine with every fast path refused, and the C++ contract suite runs every case on it as well, so the
walk, the read-and-write copy loop, fsync and the `unsupported` answer of every `refuse` and condemn without a
no-replace rename are proven on every platform. Stage 27 `at-one-open` is in: an `at` location's parent is
opened by path in one call (in pieces past `PATH_MAX`), the system resolving it, so `status` through `at` is one
open and one stat at any depth where it had been three calls a component; containment and its identity check
stay `beneath`'s. Stage 28 `face-first-offers` is in: `list_directory` reads its directory once, storing entries
as it counts them; every face first offers 1,024 entries and 1 MiB of a file and never zero-fills what the core is
about to write (the C++ `read_file` answers `Bytes`, a `std::vector<std::byte>` with a default-initialising
allocator); the engine reads the root's identity once at creation; and an `at` location's leaf opened as a
directory, like a directory `create_directories` has just made, is opened with nothing checked around the open,
containment and its identity check staying `beneath`'s. `0.1.0` is tagged when every face is green on every
platform.
