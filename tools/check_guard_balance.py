#!/usr/bin/env python3
"""Check that every `#if defined(_WIN32)` / `#else` pair opens and closes the
same number of braces.

Shared sources are compiled on two platforms, and only one branch of each
platform guard is visible to any single compiler. A brace that exists in only
one branch therefore compiles fine on the platform that has it and silently
swallows the rest of the file on the other -- which surfaces as a cascade of
nonsense errors hundreds of lines later. This check runs both branches through
the same brace counting and reports the guards that disagree.

Usage: tools/check_guard_balance.py [PATH ...]   (files or directories)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

WIN_GUARD = re.compile(r"^\s*#\s*(?:if\s+defined\s*\(\s*_WIN32\s*\)|ifdef\s+_WIN32)\s*$")
ANY_IF = re.compile(r"^\s*#\s*if")
ANY_ENDIF = re.compile(r"^\s*#\s*endif")
ANY_ELSE = re.compile(r"^\s*#\s*(?:else|elif)")

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}


def strip_noise(line: str) -> str:
    """Remove comments and string/char literals so braces are counted correctly."""
    line = re.sub(r"//.*", "", line)
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
    return line


def brace_net(lines: list[str]) -> int:
    net = 0
    for line in lines:
        stripped = strip_noise(line)
        net += stripped.count("{") - stripped.count("}")
    return net


def check_file(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    problems: list[str] = []
    index = 0
    while index < len(lines):
        if not WIN_GUARD.match(lines[index]):
            index += 1
            continue

        start = index
        depth = 0
        branch = "win"
        bodies: dict[str, list[str]] = {"win": []}
        cursor = index
        while cursor < len(lines):
            line = lines[cursor]
            if ANY_IF.match(line):
                depth += 1
            elif ANY_ENDIF.match(line):
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and ANY_ELSE.match(line):
                branch = "other"
                bodies.setdefault(branch, [])
                cursor += 1
                continue
            if depth >= 1 and cursor > start:
                bodies.setdefault(branch, []).append(line)
            cursor += 1

        win_net = brace_net(bodies.get("win", []))
        other_net = brace_net(bodies.get("other", []))
        if win_net != other_net:
            problems.append(
                f"{path}:{start + 1}: platform branches are unbalanced "
                f"(windows {win_net:+d}, other {other_net:+d} braces)"
            )
        index = cursor + 1

    return problems


def iter_sources(targets: list[str]) -> list[Path]:
    files: list[Path] = []
    for target in targets:
        path = Path(target)
        if path.is_dir():
            files.extend(sorted(p for p in path.rglob("*") if p.suffix in SOURCE_SUFFIXES))
        else:
            files.append(path)
    return files


def main(argv: list[str]) -> int:
    targets = argv[1:] or ["src"]
    problems: list[str] = []
    files = iter_sources(targets)
    for path in files:
        problems.extend(check_file(path))

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f"check_guard_balance: {len(problems)} unbalanced guard(s)", file=sys.stderr)
        return 1
    print(f"check_guard_balance: {len(files)} file(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
