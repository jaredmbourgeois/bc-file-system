# bc-file-system — agent instructions

This file is the single home of the repository's standards, written for a reader, human or agent, who has
nothing but this checkout. `CLAUDE.md` imports it; `README.md` is the user's documentation and repeats none of it.

bc-file-system is a C23 file-system core behind one header, with C++, Swift and Rust faces, in one repository:
a file system that code is *handed* instead of one it reaches for. It keeps three rules itself, everywhere, tests
included — **inject, never reach**, **no default arguments** and **no macros** — and a change that bends one is
wrong even when it compiles and passes.

## Contents

- [Layout](#layout)
- [Inject, never reach](#inject-never-reach)
- [No default arguments](#no-default-arguments)
- [No macros](#no-macros)
- [Code standards](#code-standards)
- [Tests](#tests)
- [Verification](#verification)
- [Changing a rule](#changing-a-rule)
- [Git policy](#git-policy)

## Layout

| Path | What it is | CMake target | SwiftPM target | Cargo crate |
|---|---|---|---|---|
| `core/include/bc_file_system/` | The public C header, included as `<bc_file_system/bc_file_system.h>` | `bc::file_system_core` | `CBCFileSystem` | — |
| `core/src/` | The C implementation: `bc_file_system.c` is the engine (the hosted tier), `path.c` the path module (the freestanding tier, with its private face `path.h`), and `platform.h` the only file that names the operating system (`.clang-tidy`: the C set) | `bc::file_system_core` | `CBCFileSystem` | — |
| `core/tests/` | The C-only test of the path module: a `.c` main over tables of cases, registered with `add_test`, so the module is proven through the public header alone | `bc_file_system_path_test` | not built | — |
| `cpp/include/bc_file_system/`, `cpp/src/` | The C++ face, a thin wrapper over the header (`.clang-tidy`: the C++ set) | `bc::file_system` | not built | — |
| `cpp/tests/` | The contract suite (the specification every face is held to, driving C through the C++ face), the C++ binding suite, the observer and path-value suites, and `support/` (the temporary directory, the descriptor-balance fixture, the generated `test_paths.hpp`) | `bc_file_system_test` | not built | — |
| `Sources/CBCFileSystem/`, `Sources/BCFileSystem/`, `Sources/BCFileSystemTestFixtures/`, `Tests/BCFileSystemTests/` | The Swift face over the C module, its test fixtures (the temporary directory and the observed engine), and its binding suite and source scan; `Package.swift` at the root for git pins | not built | `BCFileSystem`, `BCFileSystemTestFixtures`, `BCFileSystemTests` | — |
| `crates/` | The Rust face: `bc-file-system` and `bc-file-system-testing`, one `Cargo.lock`, `publish = false` (arrives at stage `rust-crates`) | — | — | `bc-file-system`, `bc-file-system-testing` |
| `fuzz/` | The libFuzzer targets, behind `BC_FILE_SYSTEM_BUILD_FUZZERS` (on in the `fuzz` preset): `path_fuzz.c` over the path module now; the ABI-argument and operation-sequence targets arrive at `locks-links-condemn` | `bc_file_system_path_fuzz` | — | — |
| `examples/consumer/` | A separate CMake project that consumes the core through FetchContent | its own project | — | — |
| `cmake/helpers.cmake` | The compile-options helper, the toolchain-knob list, the build posture, the four-way header check | included by the root | — | — |
| `scripts/check_house_rules.py` | The guard for the three rules, over every language here | run by `scripts/verify.sh` | — | — |
| `scripts/check_build_parity.py`, `scripts/check_disabled_cases.py` | The three builds compile the same C the same way; the disabled-case count against its pin `scripts/disabled_cases_pin.txt` | run by `scripts/verify.sh` | — | — |
| `scripts/verify.sh` | The one verify script the IDE and CI both call | — | — | — |
| `.forgejo/workflows/ci.yml` | CI: one job per leg of `scripts/verify.sh` | — | — | — |

## Inject, never reach

**A function, free or member, is handed everything it uses from outside itself.** That means the file system,
the environment, the clock and sleeping, randomness and ids, the network, the working directory, the temporary
directory, process facts (pid, host name, arguments, standard input, spawning, exiting), standard output and
standard error, the locale and the time zone.

A function never obtains one of these through a static or global accessor: in C `getenv`, `open`, `time(NULL)`,
`getcwd`; in C++ `std::filesystem::exists`, `std::chrono::system_clock::now`, `std::random_device`, `std::cout`;
in Swift `FileManager.default`, `ProcessInfo.processInfo`, `Date()`, `UUID()`; in Rust `std::env::var`,
`SystemTime::now`, `std::fs::read`; in Python `os.environ`, `Path.cwd()`, `time.time()`.

- **A value where a value will do, a seam where the code must act.** Logic that needs a fact takes the fact
  (`now`, `current_directory`). Logic that must act takes the thing that acts: one function for one operation,
  an interface for several. In C that is an opaque handle created by one live factory; in C++ a `FileSystem`
  holding an `Engine&`; in Swift a struct holding the engine; in Rust a trait.
- **A door takes the opened resource, never a path it opens itself.** A function that opens its own file from a
  path has gone around whatever file system its caller was handed, so a test's fault gate never sees it.
- **Naming a live value is itself a reach.** `bc_file_system_engine_create_live()`, `Engine::live()` and
  `Engine.live()` wrap the platform, so logic or a test that calls one has reached through a wrapper.

**A reach is legal at exactly four kinds of site.** Anything else is a finding.

1. **The composition root**: a program's `main`, and nothing below it. It is the only place a live value is
   named. In this repository: the `main` of `examples/consumer/main.c`, which creates and releases a live engine.
2. **The body of a seam's one live implementation.** In this repository: `core/src/bc_file_system.c` and
   `core/src/platform.h` (the engine and the platform operations it names), and `cpp/src/engine.cpp`, the one
   C++ file that names `bc_file_system_engine_create_live`.
3. **A test, for what the test owns**: a fixed clock, and the file system in a temporary directory the test
   created. Never another live value, the wall clock, a sleep, the environment, the home directory or the
   network. In this repository: `cpp/tests/support/temp_directory.cpp` (the C++ tests' own directory under the
   root `test_paths.hpp` hands in) and `cpp/tests/support/descriptor_balance.cpp` (the open-descriptor sweep the
   fixture takes before and after every case); `Sources/BCFileSystemTestFixtures/TemporaryDirectory.swift` (the
   one place `swift test` reads where its root is, and the only Swift file that names `FileManager`).
4. **Ambient diagnostics installed once at a root**, such as a logger, held as a written, closed list. This
   repository's list is empty: the library logs nothing and reports through its results.

The legal files are a closed list, written down in the guard: `REACH_IS_LEGAL_IN` in
`scripts/check_house_rules.py`, one entry per language. A `reach-ok: <reason>` comment documents a reach inside
one of those files and is a finding anywhere else. **An exemption added anywhere else to make a finding pass is
the finding.** A new legal site is a change to this section first (see [Changing a rule](#changing-a-rule)).

**The test double for a file system is the real file system**, in a temporary directory, behind the engine's
observer, run through the same contract suite as the live engine; an in-memory imitation is a second set of
rename, symlink and permission rules to keep honest, invisible to another language reading the same paths. The
observer is installed by one factory, `bc_file_system_engine_create_observed_for_tests` (`Engine::observed_for_tests()`
in C++, `FileSystem.observed(root:)` in Swift), and the guard's `test-only-factory` leg confines its callers to the
header, the core, the one wrapper per face, `cpp/tests/`, `Sources/BCFileSystemTestFixtures/` and `Tests/`.
**In a script, the path handed in is the injection**: a function may open the path and write the stream it
was given, and `main(argv, …)` reads the command line and passes everything down.

## No default arguments

**No declaration in this repository lets a caller omit a value**, in production code and in test code. A
default makes omission compile, so a call site, or a test meant to be hermetic, silently takes a value nobody
chose: the durability that was `written` because nobody said, the mode that was `0600` because that is what
the helper happened to use.

A call into an interface this repository does not own (the standard library, a test framework, CMake) is
outside the rule.

**The forms. Each is the same defect.**

- A parameter default: `Durability durability = Durability::written` in C++, `durability: Durability = .written`
  in Swift, `def scan(root, strict=False)` in Python.
- A same-named overload or convenience initializer that fills an argument in and forwards; in Rust a `new()`
  beside `with_x()`.
- An argument whose empty value the body replaces: `value_or(…)`, `?? fallback`, `unwrap_or(…)`, `x or default`,
  whether the caller omitted it or passed `nullopt`, `nil`, `None` or `{}`.
- A default moved out of the signature into a field of the options struct the function takes; in Rust a
  hand-written `impl Default` that supplies a dependency or a policy value, and an `Option<Dep>` with a live
  fallback.
- A template or generic parameter defaulted to a dependency.
- A Swift enum associated value or macro parameter with `= value`.
- In C, where the language has no defaults: a `NULL` or `0` argument that the function quietly replaces with a
  value of its own, and a zeroed struct field read as a choice. (A `NULL` buffer with a capacity of 0 is not
  that: it is the stated way to offer no buffer. Zero is never a valid enum input.)

**The exceptions. This list is closed.**

- A caller-context capture, where the default is the mechanism: `std::source_location::current()` in C++; in
  Swift `#file`, `#fileID`, `#filePath`, `#function`, `#line`, `#column`, `#dsohandle`, `#isolation` and
  `#_sourceLocation`; in Rust `#[track_caller]` carries no default at all.
- A Python method marked `@override` that must repeat its base's defaults.

**Named factories.** A function with its own descriptive name may fix the values its name accounts for:
`Permissions::exactly(mode)`, `Permissions::preserve_or(mode)`, `FileSystem.observed(root:)`. A named factory is
never an overload of a constructor, its own parameters are all required, and one that fixes an effect says so
in its name. There is no preset value to copy and override: a field added to one later would reach every user
of it silently.

**Not covered by the rule:** an option of a command line or a build, whose default is declared once, in the
parser that documents it (`argparse` in the guard, `option()` in `CMakeLists.txt`) and is the safe side where
it switches an effect; a C++ template default that names no dependency; `#[derive(Default)]` on Rust data
whose empty value is its only reading.

**Decided for this repository:**

- **Two values of one type, side by side, are a struct of named fields** (`ByteRange{.offset = 0, .length = 512}`,
  not two integers), and **an effect switch is a two-case enum, never a bare `bool`**: adjacent positionals of
  one type swap silently, and `f(path, true)` says nothing at the call site.
- **A field a caller must state has no silent zero.** In C++ a `Required<T>` field left out of a designated
  initializer does not compile; in C a struct the interface takes is one whose zero is invalid for every field,
  and the implementation refuses it with `invalid_argument`; in Swift the memberwise `init` has no defaults;
  in Rust the options struct has no `Default`.
- **A sweep that inlines an inherited default marks the site** with a comment `swept-default: <parameter>`
  until someone has decided the value. A marker, wrapper, overload or factory added only to make a finding
  pass is not a fix.

## No macros

**Authoring a macro is forbidden by default, in every language.** A macro is invisible to the type checker, to
the debugger and to a reader of the call site, and three build systems compiling one core cannot keep one
definition in sync. Scoped per language:

- **C and C++ author none.** An integer constant is an `enum` (`static const int` is not a constant expression
  in C); a non-integer constant a `constexpr` object or `static const`; a function-like shape `static inline`;
  a build-time value a generated source (a `configure_file`d `.in`) or an argument; caller location
  `std::source_location`; conditional compilation one internal `platform.h` with whole-file-guarded bodies.
- **CMake authors no `macro()`** and no compile definition beyond the toolchain-knob list.
- **Rust authors a `macro_rules!` only where a function or generic cannot express it** (variadic composition,
  `include_str!(concat!())`), and never a proc-macro crate.
- **Swift authors no `macro` declaration.**

Using a macro someone else defines (`assert`, `errno`, `offsetof`, `TEST`, `println!`, `#Preview`) is not
authoring one. **Defining a name the toolchain or a dependency documents as its knob is using, not
authoring**: `_GNU_SOURCE`, `_FILE_OFFSET_BITS`, `_FORTIFY_SOURCE`, `_GLIBCXX_ASSERTIONS`, `NDEBUG` and their
kind. The knob list lives once, as `BC_FILE_SYSTEM_TOOLCHAIN_KNOBS` in `cmake/helpers.cmake` (`_GLIBCXX_ASSERTIONS`
in a Debug configuration), with `_FORTIFY_SOURCE=2` beside it in the `hardened` posture; the guard's
`TOOLCHAIN_KNOBS` holds those two and the two the one Linux translation unit defines at its top, `_GNU_SOURCE`
and `_FILE_OFFSET_BITS`. That list is the only `-D` the CMake helper, the only `.define` in `Package.swift` and the
only `.define` in `build.rs` may pass. A feature-test or width macro is defined at the top of the one translation
unit that needs it, guarded by the platform name.

**The C preprocessor is confined to this closed list:** `#include`; `#pragma once`; `#if`/`#ifdef`/`#elifdef`
conditionals on a predefined platform or compiler name (`__linux__`, `__APPLE__`, `__ANDROID_API__`,
`__EMSCRIPTEN__`, `__GLIBC__`, `__cplusplus`) or on a system header's availability (`__has_include`), around
whole declarations or whole files; `#error`; the translation-unit-top knob definition above; expanding
standard macros; diagnostic push/pop around a third-party header. An `#ifndef` include guard is a `#define`,
and `#pragma once` is the guard.

**Exceptions are held by the guard, by name**: `MACRO_EXCEPTIONS` in `scripts/check_house_rules.py` lists
each admitted macro as (file, name, reason) under a pinned count that only falls. The source line carries
`macro-ok: <reason>` as the cross-reference; the list is the authority, and a marker without an entry, an
entry without a marker, and an entry whose macro is gone are each findings. This repository's list is empty,
and its pin (`macro_exception_pin`) is 0.

## Code standards

**Readability is a hard requirement.** Write for a reader fluent in programming but not in this language:
someone who knows Swift should read the C once, top to bottom, and understand it. When a construct would make
that reader stop and look something up, choose the clearer form even if it is a few lines longer, or add a
comment that explains it. Brevity is never worth obscurity.

- **Comments explain why, never what.** Self-document with names; keep comments for intent, for the reason a
  step is in the order it is, for the system behaviour a line depends on.
- **Name things in full** (`directory`, not `dir`; `descriptor`, not `fd`) and **match the code around you**.
- **Validate at the boundary, trust inside.** Arguments are checked where they enter (every public C function
  checks every pointer and enum value); pure logic does not re-check them.
- **No new dependency without a reason written beside it.** The library depends on the C standard library and
  POSIX. A test framework is a dependency of the tests only and never reaches a consumer.

### C

- **C23 in its GNU dialect** (`C_STANDARD 23` with `C_EXTENSIONS ON`; `.gnu2x` where a tool knows only that
  spelling); never `_POSIX_C_SOURCE`. The floor is checked once, in `platform.h`, with
  `#if __STDC_VERSION__ < 202000L || !__has_include(<stdckdint.h>)` and `#error` (GCC 14 reports 202000L under
  `gnu23`; Apple clang 16 reports it too but lacks `<stdckdint.h>`, so the header test names that refusal).
- **The allowlist in `.c` files and private headers**: `nullptr`, `bool`/`true`/`false`, `static_assert`, the
  `[[…]]` attributes, `constexpr` objects (non-integer only), fixed-type enums, `= {}`, `<stdckdint.h>`, digit
  separators, binary literals, `__has_include`, labels before declarations, `#elifdef`, `memccpy` — not `auto`,
  `typeof`, `thread_local`, `restrict`, `unreachable()`, `#embed`, `<stdbit.h>`, `<threads.h>`, `_BitInt`, VLAs
  (each exclusion is a measured fact of the floor compilers: absent on Apple, musl or GCC 14, undefined behaviour
  on misuse, or a width hidden in a codebase whose hazard is width).
- **The public header is a common subset**, parsed as gnu11 by Swift's importer and as C++20 by C++ consumers:
  declarations only; `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`; opaque `typedef struct x x;`; explicit `(void)`;
  a comment stating the contract at every pointer parameter; no `[[nodiscard]]`, `nullptr`, `constexpr`,
  `typeof`, `restrict`, `[static N]`, flexible arrays or compound literals. The build compiles it four ways
  (strict C11, gnu11 as the importer does it, C++20, C++23) on every run.
- **One cleanup idiom**: `[[gnu::cleanup]]` on the owned-resource structs (`owned_descriptor`,
  `owned_directory_stream`, `owned_buffer`), with `release_*` for transfer out. No `goto`, no `alloca`, no VLA.
- **Errors are enums and integers**: every public function returns a status and fills a caller-owned error
  struct of fixed-width fields (the kind, the operation, the step, the system code, a bounded name); the faces
  render text. A contract violation (NULL, an out-of-range enum, NUL in a path) returns `invalid_argument` and
  touches nothing; `assert` only on `static` internals; no `abort`, `exit` or `unreachable()` in the library;
  the library allocates nothing for answers and only bounded scratch.
- **Every external symbol starts `bc_file_system_`; everything else is `static`.** Strict prototypes; no pointer
  hidden in a typedef; `#pragma once`; own header first.
- **Banned functions** (`strcpy`, `sprintf`, `strtok`, `atoi`, `strerror`, `tmpnam`, `chdir`, `umask`,
  `getcwd`, `realpath`, `readdir_r`, `system`, `getenv`, `signal`, stdio in the core, `stat`-then-`open`,
  `malloc(n * size)`, `alloca`, `setjmp`) each have a replacement in `core/.clang-tidy`, one row per function
  with its replacement and its reason, and `clang-tidy` reports each use.
- **The build posture is one CMake cache value**, `BC_FILE_SYSTEM_BUILD_POSTURE`: `none`, `hardened` or
  `sanitized` (`memory-sanitized` on Linux), never two at once because each blinds the other; SwiftPM and `cc`
  pass none.

### C++

- **C++20**, compiled with `-Wall -Wextra -Wpedantic`, as errors in this repository's own build.
- **No exception crosses the interface.** Every operation is `noexcept` and returns a `Result<T>`: a value or
  an `Error`. The library contains no `throw`, no `try` and no `catch`.
- **`[[nodiscard]]`** on every function whose result matters (`Result` is `[[nodiscard]]` as a type), and
  **trailing return types** throughout: `auto read_file(…) noexcept -> Result<…>`.
- **Names**: `snake_case` for functions, variables and enumerators, `PascalCase` for types, `snake_case_` for
  private members. **`explicit`** on single-argument constructors; each stated exception says why in its header.
- **Headers** start with `#pragma once` and are included as `<bc_file_system/x.hpp>` everywhere, inside the
  library too. Includes come in blocks, each sorted: the header the file implements; operating-system and
  third-party headers; standard library headers; this repository's headers; headers local to the directory.
- **Ownership is RAII.** No `new`, no `delete`; every descriptor and directory stream is owned by an object
  that closes it on every return path.
- **Nothing in the face is virtual.** `FileSystem` holds an `Engine&` and each method is one call into the core;
  the test double is the observed engine below the core, so there is nothing to substitute on the C++ side.
  Hold a `FileSystem&`.
- **Formatting** is `clang-format` with the repository's `.clang-format`, run over the C and C++ you touch.

### Swift

- **Swift 6 language mode, strict concurrency on.** The package imports only the C header: no C++
  interoperability mode, so no consumer has to switch it on.
- **Errors are typed**: every function is `throws(FileSystemError)`; across a continuation, resume a non-throwing
  continuation with `Result<T, FileSystemError>` and `.get()`.
- **The face is an engine-holding struct**, `struct FileSystem: Sendable { let engine: Engine }`, with
  `Engine.live()` here and `FileSystem.observed(root:)` in the `BCFileSystemTestFixtures` product, which answers
  an `ObservedFileSystem` (the file system, its directory, the faults and the step counts). `Engine` is a `final class`
  that is `@unchecked Sendable` with the proof written at the class and the release in `deinit`. A closure
  witness is written only where a caller substitutes closures.
- **Formatting** is `swift-format` with the repository's `.swift-format` (4-space indentation, 120 columns, the
  C++ file's shape), run over the Swift you touch.
- **The functions are synchronous**; the caller owns the scheduling, and an async view takes its executor as a value.
- **The source scan runs under `swift test`**: a test in `Tests/BCFileSystemTests/` scans `Sources/` and `Tests/`
  for reaches, parameter defaults and `macro` declarations, with planted positives and near misses, and fails on an empty scan.

### Python

- Standard library only, Python 3.9 or later, fully type-annotated: `mypy --strict`, `ruff check` and
  `ruff format --check` pass at a line length of 120.
- `main(argv, output, errors)` is the composition root, under `if __name__ == "__main__":`; functions take
  what they read and return values, and nothing below `main` reads the environment or the clock.
- A script's defaults live in its `argparse` parser and nowhere else; every guard has `--selftest`.

### Shell

- `#!/usr/bin/env bash` with `set -euo pipefail`; `shellcheck` clean; functions take every value as an argument
  (no `${1:-default}`), and `main "$@"` at the bottom is the only reader of the environment.

## Tests

- **The contract suite is the specification.** `cpp/tests/contract_test.cpp` runs every case against the live
  engine and the same engine behind the observer, driving C through the C++ face; new behaviour gets its case
  there. Each face adds a binding suite (`cpp/tests/binding_test.cpp`, `Tests/BCFileSystemTests/BindingTests.swift`,
  the crate's at `rust-crates`): every operation reaches the core once, the error mapping is total, one ABI round
  trip per value type. `cpp/tests/observer_test.cpp` holds the observer's own cases (a fault takes effect at the
  scripted step; a live engine refuses the observer calls with `invalid_argument`).
- **The path module is tested in C first**: `core/tests/path_test.c` is a `.c` main over tables of cases
  (every public path function, every refusal, every edge), registered with `add_test`, and the C++ suite
  `cpp/tests/path_test.cpp` runs the same table through the face. `fuzz/path_fuzz.c` is a libFuzzer target over
  the module, built by the `fuzz` preset (clang with libFuzzer: Homebrew LLVM on a Mac) and run for ten seconds
  per target by the `fuzz-smoke` leg; its invariants are asserted with a trap, never a print.
- **A failure is provoked for real where that is possible** (a missing file, a symlink, a directory without
  permission) and injected through the observer where it is not (the disk filling up mid-write).
- **A test checks its outcome without the code under test**, through the test support's raw helpers, and
  **owns one temporary directory** under a root handed to the test binary (the generated `test_paths.hpp`
  in C++; the fixtures' legal site in Swift), removed when it ends. **Never sleep to synchronize, and never read
  the wall clock.**
- **A case that cannot pass yet is disabled, never deleted**: a gtest case is written complete and prefixed
  `DISABLED_` with the stage that turns it on in the comment above; a Swift Testing case carries
  `.disabled("stage N <slug>")`. `scripts/check_disabled_cases.py` counts both against
  `scripts/disabled_cases_pin.txt`, and the pin only falls: a case brought back to life lowers it in the same
  commit, and a case disabled to make a run pass is a finding.
- **A guard must be able to fail.** The guard scripts have `--selftest` and the Swift scan planted cases for
  every rule; a new rule comes with a plant that proves it fires and a near miss that proves it does not.

## Verification

"The build passes" means `scripts/verify.sh` exits 0. The IDE run configuration and CI both call that one
script and nothing else, so "green locally" and "green in CI" cannot drift. With no arguments it runs every
leg, in order; CI calls it once per job with that job's leg names:

```sh
bash scripts/verify.sh build-debug        # cmake --preset debug, build, ctest
bash scripts/verify.sh build-sanitized    # the same under the sanitized posture
bash scripts/verify.sh header-four-ways   # the public header as strict C11, gnu11, C++20 and C++23
bash scripts/verify.sh path-freestanding  # the path module as a freestanding object: -ffreestanding on the host, bare wasm32 where the compiler has it
bash scripts/verify.sh guards             # each guard's --selftest, then its scan of the tree
bash scripts/verify.sh consumer           # examples/consumer through FetchContent, from this checkout
bash scripts/verify.sh swift              # swift test
bash scripts/verify.sh clang-tidy         # core/ and cpp/ under their own .clang-tidy; report-only this stage
bash scripts/verify.sh fuzz-smoke         # each libFuzzer target for ten seconds under the fuzz preset (clang with libFuzzer)
```

CI runs it on Linux (Debian 13: GCC 14 and clang 19, Swift 6.4); macOS, iOS and the cross targets (Android,
Emscripten, musl) run by hand until CI has runners and packages for them, and so does `fuzz-smoke`, which needs
clang's compiler-rt (named in `ci.yml`'s gated list beside the clang sanitized leg).

## Changing a rule

The guard's named lists are the only legal way to admit a reach outside the legal files or a macro:
`REACH_IS_LEGAL_IN`, `MACRO_EXCEPTIONS` with its pin, and `TOOLCHAIN_KNOBS`. A new legal site, a new
exception or a new knob is a change to the matching section of this file first, in the same commit as the
list entry; an entry with no sentence here is a finding at review. A marker in source (`reach-ok:`,
`macro-ok:`) is the cross-reference to that entry, never the authority.

## Git policy

- **Never run a destructive git operation**: `git reset`, `git checkout -- <path>`, `git restore`,
  `git clean`, `git stash drop`, `git branch -D`, or a force-push. If you think you need one, stop and ask.
- **An agent does not commit.** Stage only when asked, and only by explicit path (never `git add -A` or
  `git add .`); draft the commit subject and leave the commit to the maintainer.
- **A commit message is its subject line only**: the lower-case, hyphenated names of the work the commit
  lands; what a body would carry has a home the next reader will find (a code comment, this file, the README).
- **Never commit build output.** `build*/`, `.build/` and `target/` are ignored; keep it that way.
