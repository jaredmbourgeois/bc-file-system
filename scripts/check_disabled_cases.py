"""check_disabled_cases.py — the disabled-case count is pinned, and the pin only falls.

A contract case that cannot pass against the core yet is written complete and disabled with the stage that turns
it on: a gtest case whose name starts `DISABLED_` under `cpp/tests/`, a Swift Testing case carrying `.disabled(`
under `Tests/`. This script counts each kind and compares it to `scripts/disabled_cases_pin.txt` (two lines:
`gtest N` and `swift N`). A count above its pin fails and names every disabled case, so a case disabled to make
a run pass is a finding; a count below its pin fails too and prints the new pin to write, so the pin records
every case brought back to life in the same commit.

Exit 0 when both counts equal their pins, 1 on a mismatch or a failed selftest, 2 when the pin file cannot be
read or the command line is wrong. Python 3.9+, stdlib only.
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

PIN_FILE: Final = "scripts/disabled_cases_pin.txt"
GTEST_DIRECTORY: Final = "cpp/tests"
SWIFT_DIRECTORY: Final = "Tests"
KINDS: Final = ("gtest", "swift")

GTEST_DISABLED_CASE: Final = re.compile(r"\bTEST(?:_P|_F)?\s*\(\s*(\w+)\s*,\s*(DISABLED_\w+)")
SWIFT_DISABLED_TRAIT: Final = re.compile(r"\.disabled\s*\(")
SWIFT_FUNCTION_AFTER: Final = re.compile(r"\bfunc\s+(\w+)")
PIN_LINE: Final = re.compile(r"^(gtest|swift) (\d+)$")
LINE_COMMENT: Final = re.compile(r"//[^\n]*")
BLOCK_COMMENT: Final = re.compile(r"/\*.*?\*/", re.DOTALL)


@dataclass(frozen=True)
class DisabledCase:
    path: str
    line: int
    name: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: {self.name}"


def blank_comments(source: str) -> str:
    """Return the source with comments replaced by spaces, newlines kept, so a commented-out case is not counted
    and a position still names its line."""

    def spaces(match: re.Match[str]) -> str:
        return "".join("\n" if character == "\n" else " " for character in match.group(0))

    return LINE_COMMENT.sub(spaces, BLOCK_COMMENT.sub(spaces, source))


def line_of(source: str, position: int) -> int:
    return source.count("\n", 0, position) + 1


def gtest_disabled_cases(source: str, path: str) -> list[DisabledCase]:
    blanked = blank_comments(source)
    return [
        DisabledCase(path, line_of(blanked, match.start()), f"{match.group(1)}.{match.group(2)}")
        for match in GTEST_DISABLED_CASE.finditer(blanked)
    ]


def swift_disabled_cases(source: str, path: str) -> list[DisabledCase]:
    """One case per `.disabled(` trait; the name is the `func` that follows it, as Swift Testing places the trait
    on the `@Test` line above the function."""
    blanked = blank_comments(source)
    cases: list[DisabledCase] = []
    for match in SWIFT_DISABLED_TRAIT.finditer(blanked):
        function = SWIFT_FUNCTION_AFTER.search(blanked, match.end())
        name = function.group(1) if function is not None else "<no func follows the trait>"
        cases.append(DisabledCase(path, line_of(blanked, match.start()), name))
    return cases


def disabled_cases(root: Path) -> dict[str, list[DisabledCase]]:
    """Return the disabled cases of each kind under its directory; a directory that does not exist holds none."""
    found: dict[str, list[DisabledCase]] = {"gtest": [], "swift": []}
    for path in sorted((root / GTEST_DIRECTORY).rglob("*.cpp")):
        found["gtest"] += gtest_disabled_cases(path.read_text(encoding="utf-8"), path.relative_to(root).as_posix())
    for path in sorted((root / SWIFT_DIRECTORY).rglob("*.swift")):
        found["swift"] += swift_disabled_cases(path.read_text(encoding="utf-8"), path.relative_to(root).as_posix())
    return found


def read_pins(source: str) -> dict[str, int] | None:
    """Return the two pins, or None when the file is not exactly `gtest N` then `swift N`."""
    lines = source.strip().split("\n")
    if len(lines) != len(KINDS):
        return None
    pins: dict[str, int] = {}
    for kind, line in zip(KINDS, lines):
        match = PIN_LINE.match(line.strip())
        if match is None or match.group(1) != kind:
            return None
        pins[kind] = int(match.group(2))
    return pins


def check_tree(root: Path, output: TextIO, errors: TextIO) -> int:
    try:
        pins = read_pins((root / PIN_FILE).read_text(encoding="utf-8"))
        found = disabled_cases(root)
    except OSError as error:
        errors.write(f"blind: {error}\n")
        return 2
    if pins is None:
        errors.write(f"blind: {PIN_FILE} is not two lines `gtest N` then `swift N`\n")
        return 2
    status = 0
    for kind in KINDS:
        count = len(found[kind])
        pin = pins[kind]
        if count > pin:
            output.write(f"{kind}: {count} disabled case(s) against a pin of {pin}; the disabled cases are:\n")
            output.writelines(f"  {case.render()}\n" for case in found[kind])
            status = 1
        elif count < pin:
            output.write(
                f"{kind}: {count} disabled case(s) against a pin of {pin}; the pin only falls: write `{kind} {count}` to {PIN_FILE}\n"
            )
            status = 1
        else:
            output.write(f"{kind}: {count} disabled case(s), pin {pin}\n")
    return status


# --- Selftest --------------------------------------------------------------------------------------------

PLANTED_GTEST: Final = (
    "TEST(Planted, DISABLED_first) {}\n"
    "// stage 13 status-and-reads\n"
    "TEST_P(Planted, DISABLED_second) {}\n"
    "TEST_F(Planted, enabled) {}\n"
    "// TEST(Planted, DISABLED_commented) {}\n"
    "/* TEST(Planted, DISABLED_blocked) {} */\n"
)
PLANTED_SWIFT: Final = (
    '@Test(.disabled("stage 13 status-and-reads"))\n'
    "func readsAFile() throws {}\n"
    "@Test\n"
    "func enabled() throws {}\n"
    '// @Test(.disabled("commented"))\n'
)


def write_planted_tree(root: Path, pin_text: str) -> None:
    (root / GTEST_DIRECTORY).mkdir(parents=True, exist_ok=True)
    (root / SWIFT_DIRECTORY / "PlantedTests").mkdir(parents=True, exist_ok=True)
    (root / "scripts").mkdir(exist_ok=True)
    (root / GTEST_DIRECTORY / "planted_test.cpp").write_text(PLANTED_GTEST, encoding="utf-8")
    (root / SWIFT_DIRECTORY / "PlantedTests" / "PlantedTests.swift").write_text(PLANTED_SWIFT, encoding="utf-8")
    (root / PIN_FILE).write_text(pin_text, encoding="utf-8")


def selftest(output: TextIO) -> bool:
    failures: list[str] = []
    gtest_cases = gtest_disabled_cases(PLANTED_GTEST, "planted_test.cpp")
    if [case.name for case in gtest_cases] != ["Planted.DISABLED_first", "Planted.DISABLED_second"]:
        failures.append(f"gtest counting: got {[case.render() for case in gtest_cases]}")
    swift_cases = swift_disabled_cases(PLANTED_SWIFT, "PlantedTests.swift")
    if [case.name for case in swift_cases] != ["readsAFile"]:
        failures.append(f"swift counting: got {[case.render() for case in swift_cases]}")

    def expect(name: str, pin_text: str, expected_status: int) -> None:
        with tempfile.TemporaryDirectory() as planted:
            root = Path(planted)
            write_planted_tree(root, pin_text)
            status = check_tree(root, io.StringIO(), io.StringIO())
            if status != expected_status:
                failures.append(f"{name}: expected exit {expected_status}, got {status}")

    expect("counts equal their pins", "gtest 2\nswift 1\n", 0)
    expect("gtest above its pin", "gtest 1\nswift 1\n", 1)
    expect("swift above its pin", "gtest 2\nswift 0\n", 1)
    expect("gtest below its pin", "gtest 3\nswift 1\n", 1)
    expect("a malformed pin file", "gtest 2\n", 2)
    expect("the kinds in the wrong order", "swift 1\ngtest 2\n", 2)

    output.writelines(f"SELFTEST FAILED: {failure}\n" for failure in failures)
    if not failures:
        output.write("selftest: 8 planted cases behaved as expected\n")
    return not failures


# --- Entry point -----------------------------------------------------------------------------------------


def main(argv: list[str], output: TextIO, errors: TextIO) -> int:
    parser = argparse.ArgumentParser(description="Check the disabled-case counts against their pins.")
    parser.add_argument("root", nargs="?", type=Path, help="the repository root to check")
    parser.add_argument("--selftest", action="store_true", help="prove that each mismatch is caught, and check nothing")
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
