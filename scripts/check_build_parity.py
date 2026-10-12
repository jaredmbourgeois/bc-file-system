#!/usr/bin/env python3
"""check_build_parity.py — the three builds compile the same C the same way.

CMake, SwiftPM and Cargo (through `cc`) each compile `core/src/*.c`, and each spells the C standard in its own
manifest: `CMAKE_C_STANDARD 23` with `CMAKE_C_EXTENSIONS ON` in `CMakeLists.txt`, `cLanguageStandard: .gnu2x` in
`Package.swift`, `.std("gnu2x")` in each `crates/*/build.rs`. This script reads every manifest that exists and
reports, per manifest, whether it agrees with the CMake root (the reference), drifts from it, or is absent. An
absent manifest is reported by name as absent, never as agreeing: `crates/` arrives at stage `rust-crates`, and
until then its line says so. It also holds CMake's core source list (`core/CMakeLists.txt`) equal to the files
under `core/src/*.c`, so a file one build compiles and another does not cannot exist.

Exit 0 when every present manifest agrees and the source lists match, 1 on a drift, 2 when blind (the CMake
root or `core/CMakeLists.txt` cannot be read, or the command line is wrong). Python 3.9+, stdlib only.
"""

from __future__ import annotations

import argparse
import io
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Final, TextIO

CMAKE_ROOT: Final = "CMakeLists.txt"
CMAKE_CORE: Final = "core/CMakeLists.txt"
PACKAGE_SWIFT: Final = "Package.swift"
BUILD_SCRIPTS_GLOB: Final = "crates/*/build.rs"
CORE_SOURCES_GLOB: Final = "core/src/*.c"
CORE_TARGET: Final = "bc_file_system_core"

CMAKE_C_STANDARD: Final = re.compile(r"set\(\s*CMAKE_C_STANDARD\s+(\d+)\s*\)")
CMAKE_C_EXTENSIONS: Final = re.compile(r"set\(\s*CMAKE_C_EXTENSIONS\s+(ON|OFF)\s*\)")
SWIFT_C_STANDARD: Final = re.compile(r"cLanguageStandard\s*:\s*\.(\w+)")
CARGO_C_STANDARD: Final = re.compile(r"\.std\(\s*\"([^\"]+)\"\s*\)")
CMAKE_CORE_LIBRARY: Final = re.compile(r"add_library\(\s*" + CORE_TARGET + r"\s+STATIC(.*?)\)", re.DOTALL)


@dataclass(frozen=True)
class Reading:
    """What one manifest spells. `standard` is None when the manifest is absent, and "unreadable" when it exists
    but spells no standard this script recognises."""

    manifest: str
    standard: str | None


def normalize_standard(spelling: str) -> str:
    """Return the one spelling this script compares: `gnu2x` and `gnu23` are the same standard (CMake and the
    older toolchains know only the `2x` name), as are `c2x` and `c23`."""
    return spelling.replace("2x", "23")


def read_cmake_standard(source: str) -> str:
    """Return the standard the CMake root spells: the number plus `gnu` when extensions are on."""
    standard = CMAKE_C_STANDARD.search(source)
    extensions = CMAKE_C_EXTENSIONS.search(source)
    if standard is None or extensions is None:
        return "unreadable"
    dialect = "gnu" if extensions.group(1) == "ON" else "c"
    return normalize_standard(dialect + standard.group(1))


def read_pattern_standard(source: str, pattern: re.Pattern[str]) -> str:
    match = pattern.search(source)
    return "unreadable" if match is None else normalize_standard(match.group(1))


def read_manifests(root: Path) -> list[Reading]:
    """Return one reading per manifest: the CMake root first, then Package.swift, then each build.rs; a manifest
    that does not exist reads as None."""
    readings = [Reading(CMAKE_ROOT, read_cmake_standard((root / CMAKE_ROOT).read_text(encoding="utf-8")))]
    package_swift = root / PACKAGE_SWIFT
    if package_swift.is_file():
        readings.append(
            Reading(PACKAGE_SWIFT, read_pattern_standard(package_swift.read_text(encoding="utf-8"), SWIFT_C_STANDARD))
        )
    else:
        readings.append(Reading(PACKAGE_SWIFT, None))
    build_scripts = sorted(root.glob(BUILD_SCRIPTS_GLOB))
    if not build_scripts:
        readings.append(Reading(BUILD_SCRIPTS_GLOB, None))
    for build_script in build_scripts:
        readings.append(
            Reading(
                build_script.relative_to(root).as_posix(),
                read_pattern_standard(build_script.read_text(encoding="utf-8"), CARGO_C_STANDARD),
            )
        )
    return readings


def cmake_core_sources(source: str) -> list[str] | None:
    """Return the `.c` files the core target lists, relative to `core/`, or None when the target is not found."""
    match = CMAKE_CORE_LIBRARY.search(source)
    if match is None:
        return None
    return sorted(token for token in match.group(1).split() if token.endswith(".c"))


def sources_on_disk(root: Path) -> list[str]:
    return sorted(path.relative_to(root / "core").as_posix() for path in root.glob(CORE_SOURCES_GLOB))


def check_tree(root: Path, output: TextIO, errors: TextIO) -> int:
    """Report every manifest and the source list; return the exit status."""
    try:
        readings = read_manifests(root)
        core_cmake = (root / CMAKE_CORE).read_text(encoding="utf-8")
    except OSError as error:
        errors.write(f"blind: cannot read a manifest: {error}\n")
        return 2
    reference = readings[0]
    if reference.standard == "unreadable":
        errors.write(f"blind: {CMAKE_ROOT} spells no CMAKE_C_STANDARD / CMAKE_C_EXTENSIONS this script can read\n")
        return 2
    drifted = False
    output.write(f"reference: {reference.manifest} spells {reference.standard}\n")
    for reading in readings[1:]:
        if reading.standard is None:
            output.write(f"absent: {reading.manifest} (no manifest to compare; it is not an agreement)\n")
        elif reading.standard == reference.standard:
            output.write(f"agrees: {reading.manifest} spells {reading.standard}\n")
        else:
            output.write(
                f"drift: {reading.manifest} spells {reading.standard}; {reference.manifest} spells {reference.standard}\n"
            )
            drifted = True

    listed = cmake_core_sources(core_cmake)
    if listed is None:
        errors.write(f"blind: {CMAKE_CORE} declares no `add_library({CORE_TARGET} STATIC …)`\n")
        return 2
    on_disk = sources_on_disk(root)
    if listed == on_disk:
        output.write(f"agrees: {CMAKE_CORE} lists exactly {CORE_SOURCES_GLOB} ({len(on_disk)} file(s))\n")
    else:
        missing = sorted(set(on_disk) - set(listed))
        extra = sorted(set(listed) - set(on_disk))
        output.write(
            f"drift: {CMAKE_CORE} source list differs from {CORE_SOURCES_GLOB}: unlisted {missing}, listed but absent {extra}\n"
        )
        drifted = True
    return 1 if drifted else 0


# --- Selftest --------------------------------------------------------------------------------------------


def write_planted_tree(
    root: Path, swift_standard: str, cargo_standard: str, listed_sources: str, files_on_disk: tuple[str, ...]
) -> None:
    (root / "core" / "src").mkdir(parents=True, exist_ok=True)
    (root / "crates" / "planted").mkdir(parents=True, exist_ok=True)
    (root / CMAKE_ROOT).write_text("set(CMAKE_C_STANDARD 23)\nset(CMAKE_C_EXTENSIONS ON)\n", encoding="utf-8")
    (root / CMAKE_CORE).write_text(
        f"add_library({CORE_TARGET} STATIC\n{listed_sources}\n    src/platform.h\n)\n", encoding="utf-8"
    )
    (root / PACKAGE_SWIFT).write_text(f"cLanguageStandard: .{swift_standard}\n", encoding="utf-8")
    (root / "crates" / "planted" / "build.rs").write_text(
        f'build.std("{cargo_standard}").compile("planted");\n', encoding="utf-8"
    )
    for name in files_on_disk:
        (root / "core" / "src" / name).write_text("int planted(void);\n", encoding="utf-8")


def selftest(output: TextIO) -> bool:
    """Plant a tree that agrees, then each drift and the absent manifest, each in a fresh directory, and check
    the verdicts."""
    failures: list[str] = []

    def expect(
        name: str,
        expected_status: int,
        swift_standard: str,
        cargo_standard: str,
        listed_sources: str,
        files_on_disk: tuple[str, ...],
    ) -> None:
        with tempfile.TemporaryDirectory() as planted:
            root = Path(planted)
            write_planted_tree(root, swift_standard, cargo_standard, listed_sources, files_on_disk)
            status = check_tree(root, io.StringIO(), io.StringIO())
            if status != expected_status:
                failures.append(f"{name}: expected exit {expected_status}, got {status}")

    expect("every manifest agrees", 0, "gnu2x", "gnu2x", "    src/a.c", ("a.c",))
    expect("Package.swift drifts", 1, "c17", "gnu2x", "    src/a.c", ("a.c",))
    expect("build.rs drifts", 1, "gnu2x", "gnu17", "    src/a.c", ("a.c",))
    expect("a source on disk is not listed", 1, "gnu2x", "gnu2x", "    src/a.c", ("a.c", "b.c"))
    expect("a listed source is not on disk", 1, "gnu2x", "gnu2x", "    src/a.c\n    src/gone.c", ("a.c",))

    with tempfile.TemporaryDirectory() as planted:
        root = Path(planted)
        write_planted_tree(root, "gnu2x", "gnu2x", "    src/a.c", ("a.c",))
        (root / PACKAGE_SWIFT).unlink()
        (root / "crates" / "planted" / "build.rs").unlink()
        report = io.StringIO()
        status = check_tree(root, report, io.StringIO())
        if status != 0 or report.getvalue().count("absent:") != 2:
            failures.append(
                f"absent manifests: expected exit 0 and two `absent:` lines, got exit {status} and: {report.getvalue()!r}"
            )
        (root / CMAKE_ROOT).write_text("project(planted)\n", encoding="utf-8")
        status = check_tree(root, io.StringIO(), io.StringIO())
        if status != 2:
            failures.append(f"an unreadable CMake root is blindness: expected exit 2, got {status}")

    output.writelines(f"SELFTEST FAILED: {failure}\n" for failure in failures)
    if not failures:
        output.write("selftest: 7 planted trees behaved as expected\n")
    return not failures


# --- Entry point -----------------------------------------------------------------------------------------


def main(argv: list[str], output: TextIO, errors: TextIO) -> int:
    parser = argparse.ArgumentParser(description="Check that CMake, SwiftPM and Cargo compile the same C the same way.")
    parser.add_argument("root", nargs="?", type=Path, help="the repository root to check")
    parser.add_argument("--selftest", action="store_true", help="prove that each drift is caught, and check nothing")
    arguments = parser.parse_args(argv)

    if arguments.selftest:
        if arguments.root is not None:
            errors.write("--selftest checks nothing: give it no root\n")
            return 2
        return 0 if selftest(output) else 1
    if arguments.root is None:
        errors.write("give the repository root to check, or --selftest\n")
        return 2
    if not arguments.root.is_dir():
        errors.write(f"blind: {arguments.root} is not a directory\n")
        return 2
    return check_tree(arguments.root, output, errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], sys.stdout, sys.stderr))
