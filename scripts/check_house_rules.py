"""check_house_rules.py — the house rules over this repository's C, C++, Rust, Swift, Python and CMake.

`scripts/verify.sh` runs it as `--selftest` followed by a scan of the tree. The rules are stated in AGENTS.md
("Inject, never reach", "No default arguments", "No macros"); this script checks the part of them a program can
check, one finding per violation, each named for its rule:

    c-/cpp-/rust-/swift-ambient-reach
                                  code obtaining something from the process around it (an operating-system
                                  header, the environment, a file opened directly, a clock, a sleep, a random
                                  source, the working or temporary directory, a process or its streams) outside
                                  the files REACH_IS_LEGAL_IN names; a `reach-ok:` marker outside them is a finding
    c-/cpp-library-stream         the library writing a process stream (tests and examples may)
    cpp-/swift-/python-parameter-default
                                  a parameter default, except the caller-context captures and a Python `@override`
    c-/cpp-authored-macro, rust-macro-rules, macro-exception-pin
                                  a `#define` or `macro_rules!` that is neither a toolchain knob nor an exception
                                  held by name in MACRO_EXCEPTIONS (pinned; the pin only falls); the line's
                                  `macro-ok: <reason>` is the cross-reference, never the authority; a proc-macro
                                  attribute is a finding
    c-/cpp-include-guard          a header guarded by `#ifndef`, or unguarded: `#pragma once` is the guard
    cmake-macro, cmake-compile-definition, cmake-preset-definition, swift-package-define, rust-build-define
                                  a CMake `macro()`; a compile definition of a name that is not a knob, in a
                                  CMake command, a `CMakePresets.json` `*_FLAGS*` cache variable, or a `.define`
    face-parity                   a function the configured C header declares that a face's files never name
    test-only-factory             the observed-engine factory (the only way to install the observer) named
                                  outside the header, the core, the one face wrapper per language and test code

The checks are lexical, over text with comments and literals blanked. What they cannot see, and a review must:
a default respelled (a forwarding overload, an in-body fallback), a reach through an alias of one's own or a
name outside the vocabulary, a definition passed through a CMake variable, Swift code inside an interpolation.

Exit 0 clean, 1 on a finding or a failed selftest, 2 when blind (a declared language with no file scanned, an
unreadable file, a configured file that is gone) or the command line is wrong. Python 3.9+, stdlib only.
"""

from __future__ import annotations

import argparse
import ast
import io
import json
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Final, TextIO


@dataclass(frozen=True)
class MacroException:
    """One authored macro admitted by name: its repository-relative file, the `#define`d or `macro_rules!`
    name, and why a function, an enum or a generated source could not express it."""

    file: str
    macro: str
    reason: str


@dataclass(frozen=True)
class Configuration:
    # The languages this repository holds ("c", "cpp", "rust", "swift", "python", "cmake"). A declared
    # language with no file scanned is blindness, so a moved directory cannot turn the guard into a pass.
    expected_languages: tuple[str, ...]
    # Repository-relative files where a reach is legal, per language: a composition root, the body of a
    # seam's one live implementation, a test's own temporary directory — nothing else. A new legal site is a
    # change to AGENTS.md first, then to this list.
    reach_is_legal_in: dict[str, tuple[str, ...]]
    # Directory prefixes of the library proper, what a consumer compiles and links; it never writes a stream.
    library_directories: tuple[str, ...]
    # Names the toolchain or a dependency documents as its knob (`_GNU_SOURCE`, `_FILE_OFFSET_BITS`,
    # `_FORTIFY_SOURCE`, `_GLIBCXX_ASSERTIONS`, `NDEBUG`): defining one is using, not authoring. The same
    # list is the only `-D` the CMake compile-options helper, a `CMakePresets.json` `*_FLAGS*` cache
    # variable, `Package.swift` and `build.rs` may pass.
    toolchain_knobs: tuple[str, ...]
    # Authored macros admitted by name, each with its reason, and the pin: the count may only fall. Raising
    # the pin is a decision recorded in the backlog, never an edit made to pass the guard.
    macro_exceptions: tuple[MacroException, ...]
    macro_exception_pin: int
    # Face parity: the public C header, the prefix every exported function starts with, and each face's files,
    # read as one text ({"C++": ("cpp/src/engine.cpp", "cpp/src/file_system.cpp"), "Swift": (…), "Rust": (…)}).
    # An empty header switches the leg off, and the scan says so.
    face_parity_header: str
    face_parity_prefix: str
    face_parity_faces: dict[str, tuple[str, ...]]
    # The test-only factory: patterns (over blanked text) that name the observed-engine factory in any language,
    # and the files or directory prefixes (ending in "/") where naming it is legal: the header and the core that
    # declare and define it, the one wrapper per face, the test fixtures and the tests.
    test_only_factory_patterns: tuple[str, ...]
    test_only_factory_is_legal_in: tuple[str, ...]


# ======================================================================================================
# Configuration — this repository. Every entry here is preceded by a sentence in AGENTS.md's matching section.
# ======================================================================================================

CONFIGURATION: Final = Configuration(
    expected_languages=("c", "cpp", "rust", "swift", "python", "cmake"),
    reach_is_legal_in={
        # The body of the seam's one live implementation (the engine, the resolution layer, the unit that makes
        # every gated system call, and the cleanup idiom they open descriptors through, its platform header and
        # the platform bodies), the one composition root in C (the consumer example's `main`), the C-only test
        # that plants a tree under the root it is handed, and the two engine fuzzers with their harness, which
        # make their roots under the directory the build hands them and count their own descriptors.
        "c": (
            "core/src/bc_file_system.c",
            "core/src/calls.c",
            "core/src/calls.h",
            "core/src/owned.h",
            "core/src/platform.h",
            "core/src/platform.c",
            "core/src/resolve.c",
            "core/src/resolve.h",
            "core/tests/resolve_test.c",
            "examples/consumer/main.c",
            "fuzz/harness.h",
            "fuzz/abi_arguments_fuzz.c",
            "fuzz/operation_sequence_fuzz.c",
        ),
        # The one C++ file that names `bc_file_system_engine_create_live` (the face's live factory), and the tests'
        # own file system: the temporary directory and the descriptor-balance fixture.
        "cpp": (
            "cpp/src/engine.cpp",
            "cpp/tests/support/temp_directory.cpp",
            "cpp/tests/support/descriptor_balance.cpp",
        ),
        # The build script (the build's composition root: cargo's environment and the directory of C sources),
        # the one Rust file that names the live factory (`Engine::live`), and the ABI smoke, which creates a live
        # engine to prove the declarations against the real core.
        "rust": (
            "crates/bc-file-system/build.rs",
            "crates/bc-file-system/src/live.rs",
            "crates/bc-file-system/tests/abi_smoke.rs",
        ),
        # The fixtures' temporary directory: the one place `swift test` reads where its root is.
        "swift": ("Sources/BCFileSystemTestFixtures/TemporaryDirectory.swift",),
    },
    # The library proper, what a consumer compiles and links; core/tests/ is the C-only test, which may print.
    library_directories=("core/include/", "core/src/", "cpp/include/", "cpp/src/"),
    # BC_FILE_SYSTEM_TOOLCHAIN_KNOBS in cmake/helpers.cmake (_GLIBCXX_ASSERTIONS), the hardened posture's
    # _FORTIFY_SOURCE, and the two the Linux translation unit defines at its top.
    toolchain_knobs=("_FILE_OFFSET_BITS", "_FORTIFY_SOURCE", "_GLIBCXX_ASSERTIONS", "_GNU_SOURCE"),
    macro_exceptions=(),
    macro_exception_pin=0,
    face_parity_header="core/include/bc_file_system/bc_file_system.h",
    face_parity_prefix="bc_file_system_",
    # The C++ face, and the Rust face's declarations; the Swift row is added at stage `swift-package`. The leg
    # looks for each C name where the face calls it, so a method named differently from its C function still
    # passes; the renamed ones, for the reader: bc_file_system_directory_open -> FileSystem::open_directory,
    # bc_file_system_status_of -> FileSystem::status, bc_file_system_directory_opened_as -> Directory::opened_as,
    # bc_file_system_engine_release / directory_release / lock_release -> the three destructors,
    # bc_file_system_observed_fault_add / faults_clear / step_count -> Engine::add_fault / clear_faults / step_count.
    # The Rust row holds every function declared (ffi.rs mirrors the whole header by hand) while the typed face
    # over them (live.rs) carries the engine, the observer and one operation; the typed face grows at stage
    # `rust-crates`, and this row then reads the typed files alone.
    face_parity_faces={
        "C++": ("cpp/src/engine.cpp", "cpp/src/error.cpp", "cpp/src/file_system.cpp", "cpp/src/path.cpp"),
        "Rust": ("crates/bc-file-system/src/ffi.rs", "crates/bc-file-system/src/live.rs"),
    },
    test_only_factory_patterns=(
        r"\bbc_file_system_engine_create_observed_for_tests\b",
        r"\bobserved_for_tests\b",
        r"\.\s*observed\s*\(\s*root\s*:",
    ),
    test_only_factory_is_legal_in=(
        "core/include/bc_file_system/bc_file_system.h",
        "core/src/bc_file_system.c",
        "cpp/include/bc_file_system/engine.hpp",
        "cpp/src/engine.cpp",
        "cpp/tests/",
        "core/tests/",
        "crates/bc-file-system/src/ffi.rs",
        "crates/bc-file-system/src/live.rs",
        "crates/bc-file-system/tests/",
        "fuzz/",
        "Sources/BCFileSystemTestFixtures/",
        "Tests/",
    ),
)

# Directories a checkout never tracks: build output, package caches, version control.
SKIPPED_DIRECTORY_NAMES: Final = (".git", ".build", ".swiftpm", "target", "__pycache__", ".mypy_cache", ".ruff_cache")
SKIPPED_DIRECTORY_PREFIX: Final = "build"
LANGUAGE_OF_SUFFIX: Final = {
    ".c": "c",
    ".h": "c",
    ".cpp": "cpp",
    ".hpp": "cpp",
    ".rs": "rust",
    ".swift": "swift",
    ".py": "python",
}
HEADER_SUFFIXES: Final = (".h", ".hpp")
REACH_MARKER: Final = re.compile(r"(//|/\*|#)\s*reach-ok:")
MACRO_MARKER: Final = re.compile(r"(//|/\*|#)\s*macro-ok:\s*\S")
# Defaults allowed because they are the mechanism: they capture the caller's own position.
CPP_CALLER_CONTEXT_CAPTURES: Final = frozenset({"std::source_location::current()"})
SWIFT_CALLER_CONTEXT_CAPTURES: Final = frozenset(
    {"#file", "#fileID", "#filePath", "#function", "#line", "#column", "#dsohandle", "#isolation", "#_sourceLocation"}
)


@dataclass(frozen=True)
class Finding:
    leg: str
    path: str
    line: int
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.leg}] {self.message}"


@dataclass(frozen=True)
class Reach:
    """One class of reach: what a finding names, the pattern over one blanked line, and a line the selftest
    plants that the pattern must find."""

    name: str
    pattern: re.Pattern[str]
    plant: str


def reach(name: str, pattern: str, plant: str) -> Reach:
    return Reach(name=name, pattern=re.compile(pattern), plant=plant)


OS_HEADERS: Final = r"unistd\.h|fcntl\.h|dirent\.h|sys/[^>]+|pthread\.h|signal\.h|csignal|poll\.h|pwd\.h|spawn\.h|ftw\.h|fts\.h|glob\.h|utime\.h|mach/[^>]+|windows\.h"
# What C code may not obtain for itself. `fs->open(` and `files.open(` are the seam itself, not a reach.
POSIX_REACH_VOCABULARY: Final = (
    reach("an operating-system header", r"#\s*include\s*<(" + OS_HEADERS + ")>", "#include <sys/stat.h>"),
    reach(
        "the environment (getenv, environ)",
        r"\b(secure_)?getenv\s*\(|\benviron\b",
        'const char *home = getenv("HOME");',
    ),
    reach(
        "a file opened directly (open, fopen)",
        r"(?<![\w.>])(::\s*)?open(at)?\s*\(|\b(fopen|freopen|tmpfile)\s*\(",
        "int descriptor = open(path, O_RDONLY);",
    ),
    reach(
        "a clock (time, clock_gettime, gettimeofday)",
        r"\b(std\s*::\s*)?time\s*\(\s*(nullptr|NULL|0)\s*\)|\b(clock_gettime|gettimeofday)\s*\(",
        "time_t stamp = time(NULL);",
    ),
    reach("a sleep", r"\b(sleep|usleep|nanosleep)\s*\(", "usleep(delay);"),
    reach("a random source", r"\b(rand|srand|arc4random\w*|getentropy)\s*\(", "int value = rand();"),
    reach("the working directory", r"\b(getcwd|chdir)\s*\(", "char *here = getcwd(buffer, size);"),
    reach(
        "the temporary directory or a temporary name",
        r"\b(tmpnam|tempnam|mkstemp|mkstemps|mkostemp|mkdtemp)\s*\(|\bP_tmpdir\b",
        "int descriptor = mkstemp(name);",
    ),
)
# C++ adds its own spellings. std::filesystem::path is a value type; every other name is an operation, or an alias that hides one.
CPP_REACH_VOCABULARY: Final = POSIX_REACH_VOCABULARY + (
    reach("a file opened directly (fstream)", r"\b(basic_)?[io]?fstream\b", "std::ifstream stream{path};"),
    reach(
        "a std::filesystem operation or alias",
        r"\bstd\s*::\s*filesystem\s*::\s*(?!path\b)\w+|\bnamespace\s+(\w+\s*=\s*)?std\s*::\s*filesystem\b",
        "if (std::filesystem::exists(path)) {}",
    ),
    reach("a clock (::now)", r"::\s*now\s*\(", "auto start = std::chrono::steady_clock::now();"),
    reach("a sleep (this_thread)", r"\bsleep_(for|until)\s*\(", "std::this_thread::sleep_for(delay);"),
    reach("a random source (random_device)", r"\brandom_device\b", "std::random_device entropy;"),
    reach(
        "the working or temporary directory (filesystem)",
        r"\b(current_path|temp_directory_path)\s*\(",
        "auto here = current_path();",
    ),
)
SWIFT_SINGLETONS: Final = r"URLSession\s*\.\s*shared|UserDefaults\s*\.\s*standard|(Locale|TimeZone|Calendar)\s*\.\s*current|Bundle\s*\.\s*main|NotificationCenter\s*\.\s*default|DispatchQueue\s*\.\s*main"
SWIFT_REACH_VOCABULARY: Final = (
    reach(
        "the file system (FileManager, Data(contentsOf:), String(contentsOf:), .write(to:))",
        r"\bFileManager\s*(\.\s*default\b|\(\s*\))|\b(Data|String)\s*\(\s*contentsOf\b|\.\s*write\s*\(\s*to\s*:",
        "let files = FileManager.default",
    ),
    reach(
        "the temporary or home directory", r"\bNS(Temporary|Home)Directory\s*\(", "let root = NSTemporaryDirectory()"
    ),
    reach(
        "the environment or the process (ProcessInfo, getenv, CommandLine)",
        r"\bProcessInfo\s*\.\s*processInfo\b|\bgetenv\s*\(|\bCommandLine\s*\.\s*arguments\b",
        "let env = ProcessInfo.processInfo.environment",
    ),
    reach(
        "a clock (Date(), Date.now, Date(timeIntervalSinceNow:))",
        r"\bDate\s*(\(\s*\)|\.\s*now\b|\(\s*timeIntervalSinceNow\b)",
        "let now = Date()",
    ),
    reach(
        "a sleep (Task.sleep, Thread.sleep)",
        r"\b(Task|Thread)\s*\.\s*sleep\b",
        "try await Task.sleep(for: .seconds(1))",
    ),
    reach(
        "a random source (UUID(), .random(in:), SystemRandomNumberGenerator)",
        r"\bUUID\s*\(\s*\)|\.\s*random\s*\(\s*in\s*:|\bSystemRandomNumberGenerator\b",
        "let id = UUID()",
    ),
    reach(
        "a shared singleton (URLSession, UserDefaults, Locale, TimeZone, Calendar, Bundle, NotificationCenter, DispatchQueue)",
        r"\b(" + SWIFT_SINGLETONS + r")\b",
        "let session = URLSession.shared",
    ),
    reach(
        "a process stream, exit or spawn (print, FileHandle.standard…, exit, Process())",
        r"(?<![\w.])(print|exit)\s*\(|\bFileHandle\s*\.\s*standard(Output|Error|Input)\b|\bProcess\s*\(\s*\)",
        'print("done")',
    ),
    # The same reaches written as implicit members, which name no type: `date: { .now }`, `session: .shared`.
    reach(
        "an implicit .now, .current, .shared, .standard or .default",
        r"(?<![\w)\]}?!])\.\s*(now|current|shared|standard|default)\b",
        "let session: URLSession = .shared",
    ),
)
# In a test, naming the live value is the reach that spreads.
SWIFT_TEST_REACH_VOCABULARY: Final = (reach(".live in a test", r"\.\s*live\b", "let files = FileSystem.live"),)
# What Rust code may not obtain for itself: the standard library's doors to the process, each a path a value or
# a seam is handed instead. `environment.var(` and `files.read(` are seams, not reaches.
RUST_REACH_VOCABULARY: Final = (
    reach(
        "the environment or the process facts (std::env)",
        r"\benv\s*::\s*(var|var_os|vars|vars_os|args|args_os|current_dir|temp_dir|home_dir|current_exe)\s*\(",
        'let home = env::var("HOME");',
    ),
    reach(
        "the file system (std::fs, File::open, OpenOptions)",
        r"\bfs\s*::\s*\w+\s*\(|\bFile\s*::\s*(open|create|create_new|options)\s*\(|\bOpenOptions\s*::\s*new\s*\(",
        "let text = fs::read_to_string(path);",
    ),
    reach(
        "a clock (SystemTime::now, Instant::now)",
        r"\b(SystemTime|Instant)\s*::\s*now\s*\(",
        "let start = Instant::now();",
    ),
    reach("a sleep (thread::sleep)", r"\b(thread\s*::\s*)?sleep\s*\(", "thread::sleep(delay);"),
    reach(
        "a random source (rand, thread_rng, random)",
        r"\brand\s*::|\bthread_rng\s*\(|\brandom\s*\(",
        "let value = rand::random::<u32>();",
    ),
    reach(
        "a process spawned, exited or identified (std::process, Command)",
        r"\bprocess\s*::\s*(exit|abort|id|Command)\b|\bCommand\s*::\s*new\s*\(",
        'let status = Command::new("ls").status();',
    ),
    reach(
        "a process stream (print!, println!, eprint!, eprintln!, dbg!, io::stdin/stdout/stderr)",
        r"\b(e?print(ln)?|dbg)\s*!|\bio\s*::\s*(stdin|stdout|stderr)\s*\(",
        'println!("done");',
    ),
)
# What the library may not write to. Tests and examples may.
STDIO_STREAM_VOCABULARY: Final = (
    reach(
        "a process stream (stdio)",
        r"\b(printf|puts|putchar|perror)\s*\(|\bf(printf|puts|putc|write)\s*\(\s*std(out|err)\b|\bstd(out|err|in)\b",
        'fprintf(stderr, "failed");',
    ),
)
CPP_STREAM_VOCABULARY: Final = STDIO_STREAM_VOCABULARY + (
    reach("a process stream (iostream)", r"\bstd\s*::\s*(cout|cerr|clog|cin)\b", 'std::cerr << "failed";'),
)


# --- Blanking -----------------------------------------------------------------------------------------

# How a string or character literal opens, per language family. "c" covers C, C++ and Rust: `"…"`, `'…'`
# (an apostrophe after a letter or digit is a digit separator or a prefix, never an opening) and `R"x(…)x"`.
# "swift": `"…"`, `"""…"""` and the raw forms with `#` on both sides.
LITERAL_OPENING: Final = {
    "c": re.compile(r'R"(?P<delimiter>[^()\\\s]{0,16})\(|"|(?<![0-9A-Za-z_])\''),
    "swift": re.compile(r'(?P<hashes>#*)(?P<quote>"""|")'),
}


def blank_comments_and_literals(source: str, language: str) -> str:
    """Return the source with comments and string and character literals replaced by spaces, newlines
    kept, so a position in the result is the same line and column in the source. `language` is "c" or
    "swift"; Swift's block comments nest, and code inside a string interpolation is blanked with its string."""
    output: list[str] = []
    index = 0
    length = len(source)

    def blank(count: int) -> None:
        nonlocal index
        for character in source[index : index + count]:
            output.append("\n" if character == "\n" else " ")
        index += count

    while index < length:
        if source.startswith("//", index):
            end = source.find("\n", index)
            blank((length if end == -1 else end) - index)
        elif source.startswith("/*", index):
            blank(block_comment_length(source, index, nested=language == "swift"))
        elif opening := LITERAL_OPENING[language].match(source, index):
            blank(literal_length(source, opening, language))
        else:
            output.append(source[index])
            index += 1
    return "".join(output)


def block_comment_length(source: str, start: int, nested: bool) -> int:
    """Return the length of the block comment opening at `start`; an unclosed one runs to the end."""
    depth = 0
    index = start
    while index < len(source):
        if source.startswith("/*", index) and (nested or depth == 0):
            depth += 1
            index += 2
        elif source.startswith("*/", index):
            depth -= 1
            index += 2
            if depth == 0:
                return index - start
        else:
            index += 1
    return len(source) - start


def literal_length(source: str, opening: re.Match[str], language: str) -> int:
    """Return the length of the literal that `opening` starts, its delimiters included. A backslash (plus
    the raw form's hashes, in Swift) escapes the character after it; a one-line literal left unclosed ends
    with its line; a raw C++ string has no escapes at all."""
    if language == "swift":
        closing = opening.group("quote") + opening.group("hashes")
        escape: str | None = "\\" + opening.group("hashes")
    elif opening.group("delimiter") is not None:
        closing = ")" + opening.group("delimiter") + '"'
        escape = None
    else:
        closing = opening.group(0)
        escape = "\\"
    ends_at_newline = not closing.startswith(('"""', ")"))
    index = opening.end()
    while index < len(source) and not source.startswith(closing, index):
        if escape is not None and source.startswith(escape, index):
            index += len(escape) + 1
        elif ends_at_newline and source[index] == "\n":
            return index - opening.start()
        else:
            index += 1
    return min(len(source), index + len(closing)) - opening.start()


def blank_line_comments(source: str, opener: str) -> str:
    """Return the source with each comment that `opener` starts (outside a double-quoted string) replaced by
    spaces, and nothing else touched — for files whose subject sits inside string literals."""
    output: list[str] = []
    for line in source.split("\n"):
        inside_string = False
        cut = len(line)
        index = 0
        while index < len(line):
            if line[index] == "\\" and inside_string:
                index += 2
                continue
            if line[index] == '"':
                inside_string = not inside_string
            elif not inside_string and line.startswith(opener, index):
                cut = index
                break
            index += 1
        output.append(line[:cut] + " " * (len(line) - cut))
    return "\n".join(output)


def line_of(blanked: str, position: int) -> int:
    return blanked.count("\n", 0, position) + 1


# --- Parameter defaults (C++ and Swift share the parameter-list reader) --------------------------------

# A parenthesis after one of these opens a condition or an operand, not a parameter list.
NOT_A_PARAMETER_LIST_AFTER: Final = (
    "if",
    "for",
    "while",
    "switch",
    "catch",
    "return",
    "sizeof",
    "alignof",
    "decltype",
    "noexcept",
    "static_assert",
    "requires",
)
TRAILING_WORD: Final = re.compile(r"[A-Za-z_]\w*$")
# `operator=`, `operator==`, `operator()`, `operator[]` and the like, at the end of the text.
OPERATOR_NAME_AT_END: Final = re.compile(r"\boperator\s*(\(\)|\[\]|[^\w\s()]{1,3})$")
# A Swift declaration keyword as a whole word that is not a member access (`.init(` is a call) or a macro name.
SWIFT_DECLARATION: Final = re.compile(r"(?<![\w.#`])(func|init|subscript|macro|case)\b")


def cpp_default_findings(source: str, path: str) -> list[Finding]:
    """Return one finding per parameter default in the C++ source."""
    blanked = blank_comments_and_literals(source, "c")
    findings: list[Finding] = []
    for opening in range(len(blanked)):
        if blanked[opening] == "(" and opens_cpp_parameter_list(blanked, opening):
            findings += default_findings_in_list(
                blanked, opening, path, "cpp-parameter-default", CPP_CALLER_CONTEXT_CAPTURES
            )
    return findings


def opens_cpp_parameter_list(blanked: str, opening: int) -> bool:
    """Return True when the parenthesis at `opening` can start a parameter list: it follows a name, a
    lambda's capture list, a template argument list or an operator name, and not a keyword whose
    parenthesis holds a condition."""
    before = blanked[:opening].rstrip()
    if not before:
        return False
    if OPERATOR_NAME_AT_END.search(before) or before[-1] in "]>":
        return True
    if not (before[-1].isalnum() or before[-1] == "_"):
        return False
    word = TRAILING_WORD.search(before)
    return word is not None and word.group(0) not in NOT_A_PARAMETER_LIST_AFTER


def swift_default_findings(source: str, path: str) -> list[Finding]:
    """Return one finding per parameter default in a `func`, `init`, `subscript`, `macro` or enum `case`
    declaration, except the caller-context captures."""
    blanked = blank_comments_and_literals(source, "swift")
    findings: list[Finding] = []
    for keyword in SWIFT_DECLARATION.finditer(blanked):
        for opening in swift_parameter_lists(blanked, keyword.group(1), keyword.end()):
            findings += default_findings_in_list(
                blanked, opening, path, "swift-parameter-default", SWIFT_CALLER_CONTEXT_CAPTURES
            )
    return findings


def swift_parameter_lists(blanked: str, keyword: str, start: int) -> list[int]:
    """Return the opening parenthesis of each parameter list the declaration at `start` owns. A `func`,
    `init`, `subscript` or `macro` has one, before its body; an enum `case` may name several values on one
    line, each list directly after a name (a `switch` case's patterns hold no `=` and so yield nothing)."""
    openings: list[int] = []
    index = start
    while index < len(blanked):
        character = blanked[index]
        if character in "{};" or (keyword == "case" and character in ":\n"):
            break
        if character == "(":
            if keyword != "case":
                return [index]
            if index > start and (blanked[index - 1].isalnum() or blanked[index - 1] in "_`"):
                openings.append(index)
            closing = matching_parenthesis(blanked, index)
            if closing is None:
                break
            index = closing
        index += 1
    return openings


def default_findings_in_list(
    blanked: str, opening: int, path: str, leg: str, captures: frozenset[str]
) -> list[Finding]:
    """Return one finding per default in the parameter list that opens at `opening`, except the captures."""
    closing = matching_parenthesis(blanked, opening)
    if closing is None:
        return []
    findings: list[Finding] = []
    for position, expression in defaults_in(blanked, opening + 1, closing):
        if "".join(expression.split()) not in captures:
            findings.append(
                Finding(
                    leg,
                    path,
                    line_of(blanked, position),
                    f"`= {expression}` lets a caller omit the value; state it at every call site",
                )
            )
    return findings


def matching_parenthesis(blanked: str, opening: int) -> int | None:
    """Return the index of the parenthesis that closes the one at `opening`, or None."""
    depth = 0
    for index in range(opening, len(blanked)):
        if blanked[index] == "(":
            depth += 1
        elif blanked[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    return None


def defaults_in(blanked: str, start: int, end: int) -> list[tuple[int, str]]:
    """Return (position, expression) for each default directly inside the parameter list that spans
    `start` to `end`: an `=` that is not inside a nested bracket and not part of another operator, with
    the expression after it up to the next parameter."""
    defaults: list[tuple[int, str]] = []
    nesting = 0
    index = start
    while index < end:
        character = blanked[index]
        if character in "([{":
            nesting += 1
        elif character in ")]}":
            nesting -= 1
        elif character == "=" and nesting == 0 and is_plain_assignment(blanked, index):
            expression_end = next_comma_outside_brackets(blanked, index + 1, end)
            defaults.append((index, blanked[index + 1 : expression_end].strip()))
            index = expression_end
            continue
        index += 1
    return defaults


def is_plain_assignment(blanked: str, index: int) -> bool:
    """Return True when the `=` at `index` stands alone: not one character of ==, !=, <=, >= or a compound assignment."""
    if index > 0 and blanked[index - 1] in "=!<>+-*/%&|^~?":
        return False
    return blanked[index + 1 : index + 2] != "="


def next_comma_outside_brackets(blanked: str, start: int, limit: int) -> int:
    nesting = 0
    for index in range(start, limit):
        character = blanked[index]
        if character in "([{":
            nesting += 1
        elif character in ")]}":
            nesting -= 1
        elif character == "," and nesting == 0:
            return index
    return limit


def python_default_findings(source: str, path: str) -> list[Finding]:
    """Return one finding per `def` or `lambda` parameter that carries a default, except on a method marked
    `@override`, which must repeat its base's defaults."""
    findings: list[Finding] = []
    for node in ast.walk(ast.parse(source, filename=path)):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            continue
        if not isinstance(node, ast.Lambda) and any(
            decorator_name(decorator) == "override" for decorator in node.decorator_list
        ):
            continue
        name = "lambda" if isinstance(node, ast.Lambda) else node.name
        for default in list(node.args.defaults) + [default for default in node.args.kw_defaults if default is not None]:
            findings.append(
                Finding(
                    "python-parameter-default",
                    path,
                    default.lineno,
                    f"`{name}` has a parameter default; pass the value at every call site",
                )
            )
    return findings


def decorator_name(decorator: ast.expr) -> str:
    if isinstance(decorator, ast.Attribute):
        return decorator.attr
    return decorator.id if isinstance(decorator, ast.Name) else ""


# --- Reaches and streams ------------------------------------------------------------------------------


def vocabulary_findings(blanked: str, path: str, leg: str, vocabulary: tuple[Reach, ...]) -> list[Finding]:
    """Return one finding per line of the blanked source that matches an entry of `vocabulary`."""
    findings: list[Finding] = []
    for line_index, line in enumerate(blanked.split("\n")):
        for entry in vocabulary:
            if entry.pattern.search(line):
                findings.append(
                    Finding(leg, path, line_index + 1, f"{entry.name}; take the value or the operation as a parameter")
                )
    return findings


def reach_findings(
    source: str, path: str, leg: str, vocabulary: tuple[Reach, ...], legal_files: tuple[str, ...], language: str
) -> list[Finding]:
    """Return the ambient reaches in one file, or none for a file where reaching is legal; outside those files
    a `reach-ok:` marker is a finding of its own, since the list, not the marker, is the authority."""
    if path in legal_files:
        return []
    findings = vocabulary_findings(blank_comments_and_literals(source, language), path, leg, vocabulary)
    for line_index, raw_line in enumerate(source.split("\n")):
        if REACH_MARKER.search(raw_line):
            findings.append(
                Finding(leg, path, line_index + 1, "`reach-ok:` is valid only in a file REACH_IS_LEGAL_IN names")
            )
    return sorted(findings, key=lambda finding: finding.line)


def stream_findings(
    source: str, path: str, leg: str, vocabulary: tuple[Reach, ...], library_directories: tuple[str, ...]
) -> list[Finding]:
    """Return the process-stream writes in one file of the library, or none for a file outside it."""
    if not path.startswith(library_directories):
        return []
    return vocabulary_findings(blank_comments_and_literals(source, "c"), path, leg, vocabulary)


def swift_reach_findings(source: str, path: str, legal_files: tuple[str, ...]) -> list[Finding]:
    in_a_test = any(part.lower() in ("test", "tests") for part in path.split("/"))
    vocabulary = SWIFT_REACH_VOCABULARY + (SWIFT_TEST_REACH_VOCABULARY if in_a_test else ())
    return reach_findings(source, path, "swift-ambient-reach", vocabulary, legal_files, "swift")


# --- Macros and include guards --------------------------------------------------------------------------

MACRO_DEFINE: Final = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.MULTILINE)
DIRECTIVE: Final = re.compile(r"^[ \t]*#[ \t]*(\w+)[ \t]*(\w*)", re.MULTILINE)
PRAGMA_ONCE: Final = re.compile(r"^[ \t]*#[ \t]*pragma[ \t]+once\b", re.MULTILINE)
MACRO_RULES: Final = re.compile(r"\bmacro_rules!\s*([A-Za-z_]\w*)")
PROC_MACRO_ATTRIBUTE: Final = re.compile(r"#\s*\[\s*proc_macro")


def include_guard_name(blanked: str) -> str | None:
    """Return the name of an `#ifndef X` / `#define X` include guard when the file's first two directives form one."""
    directives = [(match.group(1), match.group(2)) for match in DIRECTIVE.finditer(blanked)][:2]
    if len(directives) == 2 and directives[0][0] == "ifndef" and directives[1] == ("define", directives[0][1]):
        return directives[0][1]
    return None


def macro_findings(
    source: str,
    path: str,
    leg: str,
    knobs: tuple[str, ...],
    exceptions: tuple[MacroException, ...],
    definition: re.Pattern[str],
) -> list[Finding]:
    """Return one finding per authored macro in the file: each `definition` match that is neither a knob nor
    an exception held by name, each exception without its `macro-ok:` cross-reference, each marker without an
    exception, each exception the file no longer defines, and each proc-macro attribute."""
    blanked = blank_comments_and_literals(source, "c")
    raw_lines = source.split("\n")
    guard_name = include_guard_name(blanked)
    excepted = {exception.macro for exception in exceptions if exception.file == path}
    defined: set[str] = set()
    findings: list[Finding] = []
    for match in definition.finditer(blanked):
        name = match.group(1)
        line = line_of(blanked, match.start())
        marked = MACRO_MARKER.search(raw_lines[line - 1]) is not None
        if name == guard_name or name in knobs:
            continue
        defined.add(name)
        if name in excepted:
            if not marked:
                findings.append(
                    Finding(
                        leg,
                        path,
                        line,
                        f"`{name}` is admitted by MACRO_EXCEPTIONS but carries no `macro-ok: <reason>` cross-reference",
                    )
                )
        elif marked:
            findings.append(
                Finding(
                    leg,
                    path,
                    line,
                    f"`macro-ok:` on `{name}` is a cross-reference, not the authority; hold the exception by name in MACRO_EXCEPTIONS",
                )
            )
        else:
            findings.append(
                Finding(
                    leg,
                    path,
                    line,
                    f"`{name}` authors a macro; write an enum, a constexpr object, a static inline function or a generated source",
                )
            )
    for name in sorted(excepted - defined):
        findings.append(
            Finding(
                leg,
                path,
                1,
                f"MACRO_EXCEPTIONS names `{name}` here, which this file no longer defines; remove the entry and lower the pin",
            )
        )
    if definition is MACRO_RULES:
        for match in PROC_MACRO_ATTRIBUTE.finditer(blanked):
            findings.append(
                Finding(
                    leg,
                    path,
                    line_of(blanked, match.start()),
                    "a proc-macro crate is authored; no repository ships one",
                )
            )
    return findings


def include_guard_findings(source: str, path: str, leg: str) -> list[Finding]:
    """Return the finding for a header guarded by `#ifndef`, or for one with no `#pragma once`."""
    blanked = blank_comments_and_literals(source, "c")
    guard_name = include_guard_name(blanked)
    if guard_name is not None:
        return [Finding(leg, path, 1, f"`#ifndef {guard_name}` guards this header; `#pragma once` is the guard")]
    if PRAGMA_ONCE.search(blanked) is None:
        return [Finding(leg, path, 1, "no `#pragma once`")]
    return []


# --- Build definitions (CMake, Package.swift, build.rs) -------------------------------------------------

CMAKE_MACRO: Final = re.compile(r"(?<![\w.])macro\s*\(", re.IGNORECASE)
CMAKE_DEFINITION_COMMAND: Final = re.compile(
    r"\b(target_compile_definitions|add_compile_definitions|add_definitions)\s*\(", re.IGNORECASE
)
CMAKE_SCOPE_KEYWORDS: Final = ("PRIVATE", "PUBLIC", "INTERFACE")
IDENTIFIER: Final = re.compile(r"^[A-Za-z_]\w*$")
DEFINE_CALL: Final = re.compile(r"\.define\(\s*\"([A-Za-z_]\w*)\"")
DEFINITION_HINT: Final = (
    "a value code reads is a generated source or an argument; a toolchain knob goes in TOOLCHAIN_KNOBS"
)


def cmake_findings(source: str, path: str, knobs: tuple[str, ...]) -> list[Finding]:
    """Return one finding per `macro()` and per compile definition of a name that is not a knob."""
    blanked = blank_line_comments(source, "#")
    findings = [
        Finding("cmake-macro", path, line_of(blanked, match.start()), "`macro()` is authored; write a `function()`")
        for match in CMAKE_MACRO.finditer(blanked)
    ]
    for match in CMAKE_DEFINITION_COMMAND.finditer(blanked):
        closing = matching_parenthesis(blanked, match.end() - 1)
        if closing is None:
            continue
        tokens = blanked[match.end() : closing].replace(";", " ").split()
        if match.group(1).lower() == "target_compile_definitions":
            tokens = tokens[1:]  # the target
        for name in cmake_definition_names(tokens):
            if name not in knobs:
                findings.append(
                    Finding(
                        "cmake-compile-definition",
                        path,
                        line_of(blanked, match.start()),
                        f"`{name}` is defined for the compiler; {DEFINITION_HINT}",
                    )
                )
    return sorted(findings, key=lambda finding: finding.line)


def cmake_definition_names(tokens: list[str]) -> list[str]:
    """Return the macro names the definition tokens name: `NAME`, `NAME=value`, `-DNAME`, and the innermost
    name of a generator expression such as `$<$<CONFIG:Debug>:NAME>`. A variable reference names nothing this
    check can see."""
    names: list[str] = []
    for token in tokens:
        if token in CMAKE_SCOPE_KEYWORDS:
            continue
        token = token.removeprefix("-D")
        if "$<" in token:
            token = token.rsplit(":", 1)[-1].rstrip(">")
        name = token.split("=", 1)[0].strip('"')
        if IDENTIFIER.match(name):
            names.append(name)
    return names


FLAG_DEFINITION: Final = re.compile(r"(?<!\S)-D([A-Za-z_]\w*)")


def preset_definition_findings(source: str, path: str, knobs: tuple[str, ...]) -> list[Finding]:
    """Return one finding per `-D<name>` not on the knob list inside a `*_FLAGS*` cache variable of a
    `CMakePresets.json` configure preset (a value is a string or a `{"type", "value"}` object). The line
    reported is the first line of the file that carries the flag."""
    findings: list[Finding] = []
    lines = source.split("\n")
    for preset in json.loads(source).get("configurePresets", []):
        for variable, value in preset.get("cacheVariables", {}).items():
            flags = value.get("value", "") if isinstance(value, dict) else value
            if "_FLAGS" not in variable or not isinstance(flags, str):
                continue
            for name in FLAG_DEFINITION.findall(flags):
                if name not in knobs:
                    line = next((index + 1 for index, text in enumerate(lines) if f"-D{name}" in text), 1)
                    findings.append(
                        Finding(
                            "cmake-preset-definition",
                            path,
                            line,
                            f"preset `{preset.get('name', '?')}` passes `-D{name}` in `{variable}`; {DEFINITION_HINT}",
                        )
                    )
    return findings


def define_findings(source: str, path: str, leg: str, knobs: tuple[str, ...]) -> list[Finding]:
    """Return one finding per `.define("NAME", …)` of a name that is not a knob, in a `Package.swift` or a `build.rs`."""
    blanked = blank_line_comments(source, "//")
    return [
        Finding(
            leg,
            path,
            line_of(blanked, match.start()),
            f'`.define("{match.group(1)}")` authors a macro; {DEFINITION_HINT}',
        )
        for match in DEFINE_CALL.finditer(blanked)
        if match.group(1) not in knobs
    ]


# --- Face parity -----------------------------------------------------------------------------------------


def declared_functions(header_source: str, prefix: str) -> list[str]:
    """Return every `prefix…(` name the C header declares, once each, in order of first appearance."""
    names: list[str] = []
    for match in re.finditer(
        r"\b(" + re.escape(prefix) + r"[a-z0-9_]*)\s*\(", blank_comments_and_literals(header_source, "c")
    ):
        if match.group(1) not in names:
            names.append(match.group(1))
    return names


def face_parity_findings(
    functions: list[str], header_path: str, faces: dict[str, tuple[str, ...]], face_sources: dict[str, str]
) -> list[Finding]:
    """Return one finding per face that does not mention a function the header declares; `face_sources` holds
    each face's files joined into one text. The walk is one direction by design: a face may add compositions; a
    header function no face reaches is the defect."""
    findings: list[Finding] = []
    for face, face_paths in faces.items():
        blanked = blank_comments_and_literals(face_sources[face], "swift" if face_paths[0].endswith(".swift") else "c")
        for name in functions:
            if re.search(r"\b" + re.escape(name) + r"\b", blanked) is None:
                findings.append(
                    Finding(
                        "face-parity",
                        ", ".join(face_paths),
                        1,
                        f"`{name}` is declared in {header_path} and absent from the {face} face",
                    )
                )
    return findings


# --- The test-only factory --------------------------------------------------------------------------------


def test_only_factory_findings(
    source: str, path: str, language: str, patterns: tuple[str, ...], legal: tuple[str, ...]
) -> list[Finding]:
    """Return one finding per line that names the observed-engine factory in a file where that is not legal: a
    legal entry is the file's own path or a directory prefix of it."""
    if any(path == entry or (entry.endswith("/") and path.startswith(entry)) for entry in legal):
        return []
    blanked = blank_comments_and_literals(source, "swift" if language == "swift" else "c")
    findings: list[Finding] = []
    for line_index, line in enumerate(blanked.split("\n")):
        if any(re.search(pattern, line) for pattern in patterns):
            findings.append(
                Finding(
                    "test-only-factory",
                    path,
                    line_index + 1,
                    "the observed-engine factory is named outside the header, the core, the face wrapper and test code",
                )
            )
    return findings


# --- The repository --------------------------------------------------------------------------------------


def tracked_files(root: Path) -> list[Path]:
    """Return every file under `root` that a checkout tracks, sorted: build output, package caches and
    version control are left out."""
    files: list[Path] = []
    pending = [root]
    while pending:
        directory = pending.pop()
        for child in sorted(directory.iterdir()):
            if child.is_dir():
                skipped = child.name in SKIPPED_DIRECTORY_NAMES or child.name.startswith(SKIPPED_DIRECTORY_PREFIX)
                if not skipped and not child.is_symlink():
                    pending.append(child)
            elif child.is_file():
                files.append(child)
    return sorted(files)


def language_of(file: Path) -> str | None:
    if file.name in ("CMakeLists.txt", "CMakePresets.json") or file.suffix == ".cmake":
        return "cmake"
    return LANGUAGE_OF_SUFFIX.get(file.suffix)


def file_findings(source: str, path: str, language: str, configuration: Configuration) -> list[Finding]:
    """Return every finding the legs of `language` raise over one file: the pure seam the scan and the selftest share."""
    legal = configuration.reach_is_legal_in
    knobs = configuration.toolchain_knobs
    exceptions = configuration.macro_exceptions
    findings: list[Finding] = []
    if language in ("c", "cpp", "swift", "rust"):
        findings += test_only_factory_findings(
            source,
            path,
            language,
            configuration.test_only_factory_patterns,
            configuration.test_only_factory_is_legal_in,
        )
    if language == "c":
        findings += reach_findings(source, path, "c-ambient-reach", POSIX_REACH_VOCABULARY, legal.get("c", ()), "c")
        findings += stream_findings(
            source, path, "c-library-stream", STDIO_STREAM_VOCABULARY, configuration.library_directories
        )
        findings += macro_findings(source, path, "c-authored-macro", knobs, exceptions, MACRO_DEFINE)
        if path.endswith(HEADER_SUFFIXES):
            findings += include_guard_findings(source, path, "c-include-guard")
    elif language == "cpp":
        findings += cpp_default_findings(source, path)
        findings += reach_findings(source, path, "cpp-ambient-reach", CPP_REACH_VOCABULARY, legal.get("cpp", ()), "c")
        findings += stream_findings(
            source, path, "cpp-library-stream", CPP_STREAM_VOCABULARY, configuration.library_directories
        )
        findings += macro_findings(source, path, "cpp-authored-macro", knobs, exceptions, MACRO_DEFINE)
        if path.endswith(HEADER_SUFFIXES):
            findings += include_guard_findings(source, path, "cpp-include-guard")
    elif language == "rust":
        findings += reach_findings(
            source, path, "rust-ambient-reach", RUST_REACH_VOCABULARY, legal.get("rust", ()), "c"
        )
        findings += macro_findings(source, path, "rust-macro-rules", knobs, exceptions, MACRO_RULES)
        if path.endswith("build.rs"):
            findings += define_findings(source, path, "rust-build-define", knobs)
    elif language == "swift":
        findings += swift_default_findings(source, path)
        findings += swift_reach_findings(source, path, legal.get("swift", ()))
        if path.endswith("Package.swift"):
            findings += define_findings(source, path, "swift-package-define", knobs)
    elif language == "python":
        findings += python_default_findings(source, path)
    elif language == "cmake" and path.endswith("CMakePresets.json"):
        findings += preset_definition_findings(source, path, knobs)
    elif language == "cmake":
        findings += cmake_findings(source, path, knobs)
    return findings


@dataclass(frozen=True)
class ScanResult:
    findings: list[Finding]
    # True when the scan cannot vouch for the tree: a declared language had no file, a file could not be
    # read, or the configuration names a file that is gone.
    blind: bool
    files_per_language: dict[str, int]


def scan_tree(root: Path, configuration: Configuration, errors: TextIO) -> ScanResult:
    """Scan the repository at `root` and return what was found and how much was read."""
    findings: list[Finding] = []
    blind = False
    counts = {language: 0 for language in configuration.expected_languages}

    configured_files = [file for files in configuration.reach_is_legal_in.values() for file in files]
    configured_files += [exception.file for exception in configuration.macro_exceptions]
    if configuration.face_parity_header:
        configured_files += [
            configuration.face_parity_header,
            *(path for paths in configuration.face_parity_faces.values() for path in paths),
        ]
    configured_files += [entry for entry in configuration.test_only_factory_is_legal_in if not entry.endswith("/")]
    for configured in configured_files:
        if not (root / configured).is_file():
            errors.write(f"blind: the configuration names {configured}, which does not exist\n")
            blind = True

    if len(configuration.macro_exceptions) > configuration.macro_exception_pin:
        message = f"{len(configuration.macro_exceptions)} exceptions against a pin of {configuration.macro_exception_pin}; the pin only falls"
        findings.append(Finding("macro-exception-pin", "MACRO_EXCEPTIONS", 0, message))

    for file in tracked_files(root):
        language = language_of(file)
        if language is None:
            continue
        path = file.relative_to(root).as_posix()
        try:
            findings.extend(file_findings(file.read_text(encoding="utf-8"), path, language, configuration))
        except (OSError, UnicodeDecodeError, SyntaxError, ValueError, RecursionError) as error:
            errors.write(f"blind: cannot read or parse {path}: {error}\n")
            blind = True
            continue
        counts[language] = counts.get(language, 0) + 1

    for language in configuration.expected_languages:
        if counts[language] == 0:
            errors.write(
                f"blind: {language} is declared in EXPECTED_LANGUAGES and no {language} file was scanned under {root}\n"
            )
            blind = True

    if configuration.face_parity_header and not blind:
        functions = declared_functions(
            (root / configuration.face_parity_header).read_text(encoding="utf-8"), configuration.face_parity_prefix
        )
        if not functions:
            errors.write(
                f"blind: {configuration.face_parity_header} declares no `{configuration.face_parity_prefix}…(` function\n"
            )
            blind = True
        else:
            face_sources = {
                face: "\n".join((root / path).read_text(encoding="utf-8") for path in paths)
                for face, paths in configuration.face_parity_faces.items()
            }
            findings.extend(
                face_parity_findings(
                    functions, configuration.face_parity_header, configuration.face_parity_faces, face_sources
                )
            )
    return ScanResult(findings, blind, counts)


# --- Selftest --------------------------------------------------------------------------------------------

# Each plant is a source and the number of findings one leg must raise over it: the violation and, beside it,
# the near miss that must pass.
CPP_DEFAULT_PLANTS: Final = (
    ("auto read(const Path& path, bool follow = false) -> Bytes;", 1),
    ("auto f(int a = 1, std::vector<int> b = {1, 2}, int c = g(3, 4)) -> void;", 3),
    ("auto make = [](int retries = 3) { return retries; };", 1),
    ("template <class T>\nauto get(const T& value,\n         int fallback = 0) -> int;", 1),
    ("auto operator()(int index, int step = 1) const -> int;", 1),
    ("auto where(int line = 3, std::source_location location = std::source_location::current()) -> void;", 1),
    ("for (std::size_t index = 0; index < count; ++index) {}", 0),
    ("while ((count = ::read(descriptor, buffer, capacity)) < 0 && errno == EINTR) {}", 0),
    ("const bool same = compare(a == b, c <= d, e != f);", 0),
    ("auto operator=(const Descriptor&) -> Descriptor& = delete;", 0),
    ("virtual auto status(const Path& path) noexcept -> Result<Status> = 0;", 0),
    ("// auto commented(int out = 1) -> void;", 0),
    ('const char* text = "auto quoted(int in_a_string = 1) -> void;";', 0),
    ('const char* raw = R"x(auto raw(int in_a_raw_string = 1) -> void;)x";', 0),
    ("EXPECT_EQ(result, 7);\nstatic_assert(sizeof(int) == 4);", 0),
)
SWIFT_DEFAULT_PLANTS: Final = (
    ("func read(path: String, follow: Bool = false) throws -> Data", 1),
    ("init(shell: Shell, fileManager: FileManager = .default, date: @escaping () -> Date = { .now })", 2),
    ("subscript(index: Int = 0) -> Int { 0 }", 1),
    ("case written(durability: Durability = .written)", 1),
    ("macro stringify(x: Int = 1) -> String", 1),
    ("func where(line: UInt = 3, file: StaticString = #filePath, function: String = #function) {}", 1),
    ("switch value { case .some(let inner): break }", 0),
    ("if case .written(let mode) = durability { use(mode == .exact) }", 0),
    ("let same = compare(a == b, c <= d, e != f)", 0),
    ("// func commented(out: Int = 1) {}", 0),
    ('let text = #"func raw(inside: Int = 1) {}"#', 0),
    ('let text = """\nfunc multiline(inside: Int = 1) {}\n"""', 0),
    ("/* func blocked(out: Int = 1) /* nested */ {} */", 0),
    ("let built = Shape.init(width: 3)", 0),
)
PYTHON_DEFAULT_PLANTS: Final = (
    ("def stale(path, now=None):\n    return now\n", 1),
    ("def scan(root, *, strict=False):\n    return strict\n", 1),
    ("key = lambda item, reverse=False: item\n", 1),
    ("class A(B):\n    @override\n    def scan(self, root, strict=False):\n        return strict\n", 0),
    ("def scan(root, strict):\n    return strict\n", 0),
)
C_REACH_NEAR_MISSES: Final = (
    "int descriptor = fs->open(path, flags);",
    "status = files.openat(directory, name);",
    "#include <errno.h>",
    "// getenv is what this file does not call.",
    'const char *advice = "call getenv at the root";',
    "int reopened = reopen(stream);",
    "const char separator = '\\'';  /* an escaped quote */ int descriptor = fs->open(path);",
)
CPP_REACH_NEAR_MISSES: Final = C_REACH_NEAR_MISSES + (
    "auto read_file(const std::filesystem::path& path) noexcept -> Result<std::vector<std::byte>>;",
    "std::chrono::sys_time<std::chrono::nanoseconds> modified_at;",
    "auto opened = open_regular_file_for_reading(path);",
    "PosixFileSystem files = PosixFileSystem::live();",
)
SWIFT_REACH_NEAR_MISSES: Final = (
    "// FileManager.default is what this file does not call.",
    'let advice = "FileManager.default belongs at the root"',
    "let stamp = clock.now",
    "case .defaultValue:",
    "let files = fileSystem.live",
    "let printed = printer(text)",
    "func exitCode() -> Int { 0 }",
)
RUST_REACH_NEAR_MISSES: Final = (
    'let home = environment.var("HOME");',
    "let text = files.read_to_string(path);",
    "// fs::read is what this crate does not call.",
    'let advice = "call env::var at the root";',
    "let started = clock.now();",
    "let printed = printer.render(text);",
    "fn sleepy(budget: Duration) -> Duration { budget }",
    "let id = random_identifier(entropy);",
    "let answer = process_answer(bytes);",
)
STREAM_NEAR_MISSES: Final = (
    "std::string text = format_error(error);",
    "// std::cout is for programs, not for the library.",
    "int stdout_copy = 0;",
)
# (source, path, expected). The admitted macro lives in `admitted.c`; every other plant uses `planted.c`.
MACRO_PLANTS: Final = (
    ("#define BUFFER_SIZE 64\n", "core/src/planted.c", 1),
    ("#  define BUFFER_SIZE 64\n", "core/src/planted.c", 1),
    ("#define _GNU_SOURCE\n", "core/src/planted.c", 0),
    ("#define admitted(x) ((x) + 1)  // macro-ok: the selftest's admitted shape\n", "core/src/admitted.c", 0),
    ("#define admitted(x) ((x) + 1)\n", "core/src/admitted.c", 1),
    ("enum { buffer_size = 64 };\n", "core/src/admitted.c", 1),
    ("#define stray 1  // macro-ok: nobody admitted this\n", "core/src/planted.c", 1),
    ("enum { buffer_size = 64 };\n", "core/src/planted.c", 0),
    ("#ifndef PLANTED_H\n#define PLANTED_H\n#endif\n", "core/include/planted.h", 0),
    ("// #define COMMENTED 1\n#undef BUFFER_SIZE\n", "core/src/planted.c", 0),
)
RUST_MACRO_PLANTS: Final = (
    ("macro_rules! combine { ($($x:expr),*) => {} }\n", "crates/planted/src/lib.rs", 1),
    ("macro_rules! admitted { () => {} }  // macro-ok: variadic composition\n", "crates/planted/src/admitted.rs", 0),
    (
        "#[proc_macro_derive(Thing)]\npub fn derive(input: TokenStream) -> TokenStream { input }\n",
        "crates/planted/src/lib.rs",
        1,
    ),
    ('// macro_rules! commented { () => {} }\nprintln!("{}", value);\n', "crates/planted/src/lib.rs", 0),
)
INCLUDE_GUARD_PLANTS: Final = (
    ("#ifndef PLANTED_H\n#define PLANTED_H\nint planted(void);\n#endif\n", 1),
    ("int planted(void);\n", 1),
    ("// comment first\n#pragma once\n#ifndef NDEBUG\n#define TRACE 1\n#endif\n", 0),
)
CMAKE_PLANTS: Final = (
    ("macro(planted_helper)\nendmacro()\n", 1),
    ("function(planted_helper)\nendfunction()\n", 0),
    ('target_compile_definitions(planted PRIVATE PLANTED_TEST_ROOT="${root}")\n', 1),
    (
        "target_compile_definitions(planted\n  PRIVATE\n  $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>\n  PLANTED_A=1 PLANTED_B)\n",
        2,
    ),
    ("add_definitions(-DPLANTED_FLAG)\n", 1),
    ("add_compile_definitions(_FORTIFY_SOURCE=2)\n", 0),
    ("# target_compile_definitions(planted PRIVATE PLANTED_COMMENTED)\nset(PLANTED_MACRO_NAME 1)\n", 0),
)
PRESET_PLANTS: Final = (
    (
        '{"configurePresets": [{"name": "hardened", "cacheVariables": {"CMAKE_C_FLAGS": "-D_FORTIFY_SOURCE=2 -DPLANTED_FLAG"}}]}',
        1,
    ),
    (
        '{"configurePresets": [{"name": "x", "cacheVariables": {"CMAKE_CXX_FLAGS_DEBUG": {"type": "STRING", "value": "-DPLANTED_A -DPLANTED_B=1"}}}]}',
        2,
    ),
    (
        '{"configurePresets": [{"name": "x", "cacheVariables": {"CMAKE_C_FLAGS": "-O2 -Wall", "PLANTED_OPTION": "-DNOT_A_FLAG"}}]}',
        0,
    ),
    (
        '{"configurePresets": [{"name": "x", "cacheVariables": {"CMAKE_C_FLAGS": "-D_GNU_SOURCE", "CMAKE_BUILD_TYPE": "Debug"}}]}',
        0,
    ),
)
DEFINE_PLANTS: Final = (
    ('cSettings: [.define("PLANTED_FLAG", to: "1")]\n', 1),
    ('cSettings: [.define("_GNU_SOURCE", .when(platforms: [.linux]))]\n', 0),
    ('build.std("gnu2x").define("PLANTED_FLAG", "1").compile("planted");\n', 1),
    ('// .define("PLANTED_COMMENTED", "1")\n', 0),
)
SELFTEST_CONFIGURATION: Final = Configuration(
    expected_languages=("c",),
    reach_is_legal_in={
        "c": ("core/src/live.c",),
        "cpp": ("core/src/live.cpp",),
        "rust": ("crates/planted/src/live.rs",),
        "swift": ("Tests/PlantedTests/TemporaryDirectory.swift",),
    },
    library_directories=("core/",),
    toolchain_knobs=("_GNU_SOURCE", "_GLIBCXX_ASSERTIONS", "_FORTIFY_SOURCE"),
    macro_exceptions=(
        MacroException("core/src/admitted.c", "admitted", "the selftest's admitted shape"),
        MacroException("core/src/admitted.cpp", "admitted", "the selftest's admitted shape"),
        MacroException("crates/planted/src/admitted.rs", "admitted", "variadic composition"),
    ),
    macro_exception_pin=3,
    face_parity_header="",
    face_parity_prefix="",
    face_parity_faces={},
    test_only_factory_patterns=(r"\bplanted_engine_create_observed_for_tests\b", r"\.\s*observed\s*\(\s*root\s*:"),
    test_only_factory_is_legal_in=("core/src/planted_core.c", "cpp/src/engine.cpp", "Tests/"),
)
# (source, path, expected) for the test-only factory: the C name and the Swift spelling, in the library, in a
# legal file, under a legal directory, and commented out.
TEST_ONLY_FACTORY_PLANTS: Final = (
    ("engine = planted_engine_create_observed_for_tests(&engine, &error);", "cpp/src/file_system.cpp", 1),
    ("engine = planted_engine_create_observed_for_tests(&engine, &error);", "cpp/src/engine.cpp", 0),
    ("engine = planted_engine_create_observed_for_tests(&engine, &error);", "core/src/planted_core.c", 0),
    ("let files = FileSystem.observed(root: root)", "Sources/Planted/Planted.swift", 1),
    ("let files = FileSystem.observed(root: root)", "Tests/PlantedTests/PlantedTests.swift", 0),
    ("// planted_engine_create_observed_for_tests is what this file does not call", "cpp/src/file_system.cpp", 0),
    ("let files = FileSystem.observedElsewhere(root: root)", "Sources/Planted/Planted.swift", 0),
)


def selftest(output: TextIO) -> bool:
    """Plant every violation and a near miss beside it, for every leg. Return True when each plant was caught
    and each near miss was passed, reporting every miss otherwise."""
    failures: list[str] = []
    planted = 0
    configuration = SELFTEST_CONFIGURATION
    knobs = configuration.toolchain_knobs
    exceptions = configuration.macro_exceptions

    def expect(leg: str, source: str, expected: int, findings: list[Finding]) -> None:
        nonlocal planted
        planted += 1
        if len(findings) != expected:
            rendered = "; ".join(finding.render() for finding in findings) or "nothing"
            failures.append(f"{leg}: expected {expected} finding(s), got {len(findings)} ({rendered}), for: {source!r}")

    def leg_findings(leg: str, source: str, path: str, language: str) -> list[Finding]:
        return [finding for finding in file_findings(source, path, language, configuration) if finding.leg == leg]

    for source, expected in CPP_DEFAULT_PLANTS:
        expect("cpp-parameter-default", source, expected, cpp_default_findings(source, "core/src/planted.cpp"))
    for source, expected in SWIFT_DEFAULT_PLANTS:
        expect(
            "swift-parameter-default", source, expected, swift_default_findings(source, "Sources/Planted/Planted.swift")
        )
    for source, expected in PYTHON_DEFAULT_PLANTS:
        expect("python-parameter-default", source, expected, python_default_findings(source, "scripts/planted.py"))

    reach_legs = (
        (
            "c-ambient-reach",
            "c",
            POSIX_REACH_VOCABULARY,
            C_REACH_NEAR_MISSES,
            "core/src/planted.c",
            "core/test/planted_test.c",
        ),
        (
            "cpp-ambient-reach",
            "cpp",
            CPP_REACH_VOCABULARY,
            CPP_REACH_NEAR_MISSES,
            "core/src/planted.cpp",
            "core/test/planted_test.cpp",
        ),
        (
            "rust-ambient-reach",
            "rust",
            RUST_REACH_VOCABULARY,
            RUST_REACH_NEAR_MISSES,
            "crates/planted/src/planted.rs",
            "crates/planted/tests/planted.rs",
        ),
        (
            "swift-ambient-reach",
            "swift",
            SWIFT_REACH_VOCABULARY + SWIFT_TEST_REACH_VOCABULARY,
            SWIFT_REACH_NEAR_MISSES,
            "Sources/Planted/Planted.swift",
            "Tests/PlantedTests/PlantedTests.swift",
        ),
    )
    for leg, language, vocabulary, near_misses, library_path, test_path in reach_legs:
        legal_file = configuration.reach_is_legal_in[language][0]
        for entry in vocabulary:
            # `.live` is a reach in a test only; everything else is one in the library and in a test, and never in the legal file.
            expect(
                leg,
                entry.plant,
                0 if entry in SWIFT_TEST_REACH_VOCABULARY else 1,
                leg_findings(leg, entry.plant, library_path, language),
            )
            expect(leg, entry.plant, 1, leg_findings(leg, entry.plant, test_path, language))
            expect(leg, entry.plant, 0, leg_findings(leg, entry.plant, legal_file, language))
        for near_miss in near_misses:
            expect(leg, near_miss, 0, leg_findings(leg, near_miss, library_path, language))
        marker = "let files = handed  // reach-ok: the selftest's stray marker"
        expect(leg, marker, 1, leg_findings(leg, marker, library_path, language))
        expect(leg, marker, 0, leg_findings(leg, marker, legal_file, language))

    stream_legs = (
        ("c-library-stream", STDIO_STREAM_VOCABULARY, "core/src/planted.c", "examples/planted.c"),
        ("cpp-library-stream", CPP_STREAM_VOCABULARY, "core/src/planted.cpp", "examples/planted.cpp"),
    )
    for leg, stream_vocabulary, library_path, example_path in stream_legs:
        for entry in stream_vocabulary:
            expect(
                leg,
                entry.plant,
                1,
                stream_findings(entry.plant, library_path, leg, stream_vocabulary, configuration.library_directories),
            )
            expect(
                leg,
                entry.plant,
                0,
                stream_findings(entry.plant, example_path, leg, stream_vocabulary, configuration.library_directories),
            )
        for near_miss in STREAM_NEAR_MISSES:
            expect(
                leg,
                near_miss,
                0,
                stream_findings(near_miss, library_path, leg, stream_vocabulary, configuration.library_directories),
            )

    for source, path, expected in MACRO_PLANTS:
        expect(
            "c-authored-macro",
            source,
            expected,
            macro_findings(source, path, "c-authored-macro", knobs, exceptions, MACRO_DEFINE),
        )
        cpp_path = path.replace(".c", ".cpp").replace(".h", ".hpp")
        expect(
            "cpp-authored-macro",
            source,
            expected,
            macro_findings(source, cpp_path, "cpp-authored-macro", knobs, exceptions, MACRO_DEFINE),
        )
    for source, path, expected in RUST_MACRO_PLANTS:
        expect(
            "rust-macro-rules",
            source,
            expected,
            macro_findings(source, path, "rust-macro-rules", knobs, exceptions, MACRO_RULES),
        )
    for source, expected in INCLUDE_GUARD_PLANTS:
        expect(
            "c-include-guard",
            source,
            expected,
            include_guard_findings(source, "core/include/planted.h", "c-include-guard"),
        )
        expect(
            "cpp-include-guard",
            source,
            expected,
            include_guard_findings(source, "core/include/planted.hpp", "cpp-include-guard"),
        )
    for source, expected in CMAKE_PLANTS:
        expect("cmake", source, expected, cmake_findings(source, "CMakeLists.txt", knobs))
    for source, expected in PRESET_PLANTS:
        expect(
            "cmake-preset-definition", source, expected, preset_definition_findings(source, "CMakePresets.json", knobs)
        )
    for source, expected in DEFINE_PLANTS:
        expect(
            "swift-package-define",
            source,
            expected,
            define_findings(source, "Package.swift", "swift-package-define", knobs),
        )
        expect(
            "rust-build-define",
            source,
            expected,
            define_findings(source, "crates/planted/build.rs", "rust-build-define", knobs),
        )

    header = "int planted_open(void);\nint planted_close(int descriptor);\nstatic int planted_close(int descriptor);\n"
    functions = declared_functions(header, "planted_")
    expect("face-parity (declared functions)", header, 2, [Finding("face-parity", "", 0, name) for name in functions])
    faces = {
        "Swift": ("Sources/Planted/Planted.swift",),
        "Rust": ("crates/planted/src/ffi.rs", "crates/planted/src/live.rs"),
    }
    sources = {
        "Swift": "planted_open(); planted_close(descriptor)",
        "Rust": "fn planted_open(); // planted_close is only mentioned here",
    }
    expect("face-parity", "faces", 1, face_parity_findings(functions, "planted.h", faces, sources))
    joined = {"Swift": "planted_open(); planted_close(descriptor)", "Rust": "fn planted_open();\nfn planted_close();"}
    expect(
        "face-parity (a face spread over two files)",
        "faces",
        0,
        face_parity_findings(functions, "planted.h", faces, joined),
    )

    for source, path, expected in TEST_ONLY_FACTORY_PLANTS:
        language = "swift" if path.endswith(".swift") else ("c" if path.endswith(".c") else "cpp")
        expect("test-only-factory", source, expected, leg_findings("test-only-factory", source, path, language))

    # The scan over a tree the selftest owns: blindness, a healthy tree, and routing by suffix.
    with tempfile.TemporaryDirectory() as planted_root:
        root = Path(planted_root)
        blind_marker = [Finding("blind", "", 0, "")]
        (root / "scripts").mkdir()
        (root / "scripts" / "planted.py").write_text("def scan(root, strict):\n    return strict\n", encoding="utf-8")
        expect(
            "blind: declared language unseen and configured files gone",
            planted_root,
            1,
            blind_marker if scan_tree(root, configuration, io.StringIO()).blind else [],
        )
        (root / "core" / "src").mkdir(parents=True)
        for name in ("live.c", "live.cpp", "planted_core.c"):
            (root / "core" / "src" / name).write_text("int live(void) { return 0; }\n", encoding="utf-8")
        (root / "cpp" / "src").mkdir(parents=True)
        (root / "cpp" / "src" / "engine.cpp").write_text("int live(void) { return 0; }\n", encoding="utf-8")
        for name in ("admitted.c", "admitted.cpp"):
            (root / "core" / "src" / name).write_text(
                "#define admitted(x) (x)  // macro-ok: the selftest's admitted shape\n", encoding="utf-8"
            )
        (root / "Tests" / "PlantedTests").mkdir(parents=True)
        (root / "Tests" / "PlantedTests" / "TemporaryDirectory.swift").write_text(
            "let root = NSTemporaryDirectory()\n", encoding="utf-8"
        )
        (root / "crates" / "planted" / "src").mkdir(parents=True)
        (root / "crates" / "planted" / "src" / "admitted.rs").write_text(
            "macro_rules! admitted { () => {} }  // macro-ok: variadic composition\n", encoding="utf-8"
        )
        (root / "crates" / "planted" / "src" / "live.rs").write_text(
            'let home = env::var("HOME");  // reach-ok: the selftest\'s live site\n', encoding="utf-8"
        )
        clean_result = scan_tree(root, configuration, io.StringIO())
        expect(
            "a healthy planted tree",
            planted_root,
            0,
            clean_result.findings + (blind_marker if clean_result.blind else []),
        )
        (root / "core" / "src" / "planted.h").write_text(
            "#ifndef PLANTED_H\n#define PLANTED_H\n#endif\n", encoding="utf-8"
        )
        expect(
            "routing a .h file to the include-guard leg",
            planted_root,
            1,
            scan_tree(root, configuration, io.StringIO()).findings,
        )

    output.writelines(f"SELFTEST FAILED: {failure}\n" for failure in failures)
    if not failures:
        output.write(f"selftest: {planted} planted cases behaved as expected\n")
    return not failures


# --- Entry point -----------------------------------------------------------------------------------------


def main(argv: list[str], output: TextIO, errors: TextIO) -> int:
    parser = argparse.ArgumentParser(description="Check the house rules over this repository.")
    parser.add_argument("root", nargs="?", type=Path, help="the repository root to scan")
    parser.add_argument("--selftest", action="store_true", help="prove that each rule can fail, and scan nothing")
    arguments = parser.parse_args(argv)

    if arguments.selftest:
        if arguments.root is not None:
            errors.write("--selftest scans nothing: give it no root\n")
            return 2
        return 0 if selftest(output) else 1
    if arguments.root is None:
        errors.write("give the repository root to scan, or --selftest\n")
        return 2
    if not arguments.root.is_dir():
        errors.write(f"blind: {arguments.root} is not a directory\n")
        return 2

    result = scan_tree(arguments.root, CONFIGURATION, errors)
    output.writelines(finding.render() + "\n" for finding in result.findings)
    if result.blind:
        return 2
    scanned = (
        ", ".join(f"{count} {language}" for language, count in sorted(result.files_per_language.items()))
        or "no source file"
    )
    parity = "on" if CONFIGURATION.face_parity_header else "off (no header configured)"
    if result.findings:
        output.write(f"house rules: {len(result.findings)} finding(s) in {scanned}; face parity {parity}\n")
        return 1
    output.write(f"house rules: clean ({scanned}; face parity {parity})\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], sys.stdout, sys.stderr))
