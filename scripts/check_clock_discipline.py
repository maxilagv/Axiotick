#!/usr/bin/env python3
"""Clock discipline gate (ADR 0014).

Bans direct std::chrono clock calls outside the single allowed translation
unit, so every duration in the codebase shares one clock base:

  - high_resolution_clock: banned everywhere, no exceptions (it is an alias
    with no guarantees; it was the source of the mixed-clock benchmarks).
  - steady_clock / system_clock: allowed ONLY in the files listed in
    ALLOWED_CLOCK_FILES (the time_utils implementation itself).

Everything else must call core::mono_now_ns() / core::wall_now_ns().

Exit code 0 = clean, 1 = violations found. Run from the repository root.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SOURCE_EXTENSIONS = {".c", ".h", ".cpp", ".hpp"}

EXCLUDED_DIR_NAMES = {".git", "build"}

# The only translation unit allowed to touch std::chrono clocks directly.
ALLOWED_CLOCK_FILES = {
    Path("backend/modules/core/src/time_utils.cpp"),
}

BANNED_EVERYWHERE = re.compile(r"high_resolution_clock")
RESTRICTED = re.compile(r"\b(?:steady_clock|system_clock)\b")


def is_excluded(path: Path) -> bool:
    return any(part in EXCLUDED_DIR_NAMES or part.startswith("build-") for part in path.parts)


def main() -> int:
    violations: list[str] = []

    for path in sorted(REPO_ROOT.rglob("*")):
        if path.suffix not in SOURCE_EXTENSIONS or not path.is_file():
            continue
        relative = path.relative_to(REPO_ROOT)
        if is_excluded(relative):
            continue

        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            violations.append(f"{relative}: unreadable ({error})")
            continue

        for line_number, line in enumerate(text.splitlines(), start=1):
            if BANNED_EVERYWHERE.search(line):
                violations.append(
                    f"{relative}:{line_number}: high_resolution_clock is banned "
                    f"(use core::mono_now_ns): {line.strip()}"
                )
            elif RESTRICTED.search(line) and relative.as_posix() not in {
                allowed.as_posix() for allowed in ALLOWED_CLOCK_FILES
            }:
                violations.append(
                    f"{relative}:{line_number}: direct chrono clock outside time_utils.cpp "
                    f"(use core::mono_now_ns/wall_now_ns): {line.strip()}"
                )

    if violations:
        print("clock discipline violations (ADR 0014):")
        for violation in violations:
            print(f"  {violation}")
        return 1

    print("clock discipline: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
