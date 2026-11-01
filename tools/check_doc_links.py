#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate documentation links and repository-relative paths.

Scans README.md, docs/**/*.md, config/README.md, src/ecat/generated/README.md and
tests/probes/**/README.md, then checks:

1. Markdown relative link targets exist (http(s), mailto, anchors and absolute
   host paths are ignored).
2. Repository-relative paths mentioned in prose or code blocks exist for the
   prefixes src/, config/, tests/, scripts/, tools/ and docs/.
   Wildcards (``*``) are skipped, and a trailing slash requires a directory.

Exit status is non-zero when anything is missing, printing
``MISSING: file:line -> path`` for every failure.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent

PATH_PREFIXES = ("src", "config", "tests", "scripts", "tools", "docs")
PATH_PATTERN = re.compile(
    r"(?<![\w.\-])(?:" + "|".join(PATH_PREFIXES) + r")/[A-Za-z0-9_.\-/]*"
)
LINK_PATTERN = re.compile(r"\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)\s*\)")
URL_PATTERN = re.compile(r"https?://\S+")
TRAILING_PUNCTUATION = ".,;:!?'\"`)]}>"
IGNORED_SCHEMES = ("http", "https", "mailto", "tel", "ftp")


def markdown_files() -> list[Path]:
    """Return the Markdown files that participate in the documentation set."""
    files: list[Path] = []
    readme = ROOT / "README.md"
    if readme.is_file():
        files.append(readme)
    docs = ROOT / "docs"
    if docs.is_dir():
        files.extend(sorted(docs.rglob("*.md")))
    for relative in ("config/README.md", "src/ecat/generated/README.md"):
        candidate = ROOT / relative
        if candidate.is_file():
            files.append(candidate)
    probes = ROOT / "tests" / "probes"
    if probes.is_dir():
        files.extend(sorted(probes.rglob("README.md")))
    return files


def relative_link_target(raw: str) -> str | None:
    """Normalize a Markdown link target, or return None when it is ignored."""
    target = raw.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    words = target.split()
    target = words[0] if words else ""
    if not target or target.startswith("#"):
        return None
    split = urlsplit(target)
    if split.scheme.lower() in IGNORED_SCHEMES:
        return None
    if target.startswith("/") or target.startswith("//"):
        return None
    path = unquote(split.path)
    return path or None


def record(
    failures: set[tuple[str, int, str]], markdown: Path, line: int, path: str
) -> None:
    failures.add((markdown.relative_to(ROOT).as_posix(), line, path))


def check_markdown(
    markdown: Path, failures: set[tuple[str, int, str]]
) -> None:
    text = markdown.read_text(encoding="utf-8")
    for line_number, line in enumerate(text.splitlines(), start=1):
        for match in LINK_PATTERN.finditer(line):
            target = relative_link_target(match.group(1))
            if target is None:
                continue
            if not (markdown.parent / target).resolve().exists():
                record(failures, markdown, line_number, target)

        # Mask URLs so their path fragments are not mistaken for repo paths.
        masked = URL_PATTERN.sub(" ", line)
        for match in PATH_PATTERN.finditer(masked):
            end = match.end()
            if masked[end : end + 1] == "*":
                continue
            path = match.group(0).rstrip(TRAILING_PUNCTUATION)
            if not path or "*" in path:
                continue
            candidate = ROOT / path
            if path.endswith("/"):
                if not candidate.is_dir():
                    record(failures, markdown, line_number, path)
            elif not candidate.exists():
                record(failures, markdown, line_number, path)


def main() -> int:
    files = markdown_files()
    failures: set[tuple[str, int, str]] = set()
    for markdown in files:
        try:
            check_markdown(markdown, failures)
        except OSError as error:
            print(f"ERROR: cannot read {markdown}: {error}", file=sys.stderr)
            return 2

    if failures:
        for file_name, line, path in sorted(failures):
            print(f"MISSING: {file_name}:{line} -> {path}")
        print(
            f"{len(failures)} missing link/path(s) in {len(files)} Markdown file(s)."
        )
        return 1

    print(f"OK: checked {len(files)} Markdown file(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
