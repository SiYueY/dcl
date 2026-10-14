#!/usr/bin/env python3
"""Check DMW's Appendix A public-API inventory against a Git source baseline.

This is deliberately dependency-free so it can run in a minimal CI checkout:

  python3 dmw/tools/check_public_api_inventory.py \
      --repo . --document docs/dmw/dmw_v1.0.md \
      --source-ref <immutable-commit>

It verifies that every public header has an Appendix A section and that every
public declaration name has a textual counterpart in that section.  It is a
freeze guard: a header addition or declaration rename/removal fails the check
and requires an explicit Appendix A review.  C++ semantic equivalence (for
example typedef expansion) remains a compiler/API-review responsibility.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path


APPENDIX_START = "# Appendix A. Complete Public API Index"
SECTION = re.compile(r"^## A\.\d+ .*?((?:`[^`]+`(?:\s*/\s*)?)+)\s*$", re.MULTILINE)
PATH = re.compile(r"`([^`]+\.hpp)`")
MANIFEST_START = "<!-- DMW_PUBLIC_API_HEADER_SHA256"
MANIFEST_END = "DMW_PUBLIC_API_HEADER_SHA256 -->"


def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True)


def appendix_sections(document: str) -> dict[str, str]:
    try:
        appendix = document[document.index(APPENDIX_START) :]
    except ValueError as error:
        raise ValueError("Appendix A heading is missing") from error

    matches = list(SECTION.finditer(appendix))
    sections: dict[str, str] = {}
    for index, match in enumerate(matches):
        body_end = matches[index + 1].start() if index + 1 < len(matches) else len(appendix)
        body = appendix[match.start() : body_end]
        for relative_path in PATH.findall(match.group(1)):
            if relative_path in sections:
                raise ValueError(f"Appendix A lists {relative_path} more than once")
            sections[relative_path] = body
    return sections


def manifest(document: str) -> dict[str, str]:
    try:
        body = document.split(MANIFEST_START, 1)[1].split(MANIFEST_END, 1)[0]
    except IndexError as error:
        raise ValueError("Appendix A SHA-256 manifest is missing") from error
    entries: dict[str, str] = {}
    for path, digest in re.findall(r"^([\w/]+\.hpp)\s+([0-9a-f]{64})$", body, re.MULTILINE):
        if path in entries:
            raise ValueError(f"manifest lists {path} more than once")
        entries[path] = digest
    if not entries:
        raise ValueError("Appendix A SHA-256 manifest is empty")
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--document", type=Path, required=True)
    parser.add_argument("--source-ref", required=True)
    args = parser.parse_args()

    repo = args.repo.resolve()
    document_path = args.document.resolve()
    document = document_path.read_text(encoding="utf-8")
    sections = appendix_sections(document)
    expected_digests = manifest(document)
    headers = sorted(
        path.removeprefix("dmw/include/dmw/")
        for path in git(repo, "ls-tree", "-r", "--name-only", args.source_ref, "dmw/include/dmw").splitlines()
        if path.endswith(".hpp")
    )

    errors: list[str] = []
    missing_sections = sorted(set(headers) - set(sections))
    extra_sections = sorted(set(sections) - set(headers))
    errors.extend(f"Appendix A lacks header section: {path}" for path in missing_sections)
    errors.extend(f"Appendix A references absent header: {path}" for path in extra_sections)
    errors.extend(
        f"Appendix A SHA-256 manifest lacks header: {path}"
        for path in sorted(set(headers) - set(expected_digests))
    )
    errors.extend(
        f"Appendix A SHA-256 manifest references absent header: {path}"
        for path in sorted(set(expected_digests) - set(headers))
    )

    for path in headers:
        source = git(repo, "show", f"{args.source_ref}:dmw/include/dmw/{path}")
        # ``git show`` preserves the source bytes but command substitution is
        # commonly used to generate manifests and strips trailing newlines.
        # Header declarations are unaffected by final line terminators, so
        # normalize only those terminators before hashing.
        actual_digest = hashlib.sha256(source.rstrip("\n").encode()).hexdigest()
        if path in expected_digests and actual_digest != expected_digests[path]:
            errors.append(
                f"{path}: header digest differs from the Appendix A reviewed source "
                f"({expected_digests[path]} != {actual_digest})"
            )

    if errors:
        print("DMW public API inventory check failed:", file=sys.stderr)
        print(*errors, sep="\n", file=sys.stderr)
        return 1
    print(f"DMW public API inventory check passed for {args.source_ref} ({len(headers)} headers).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
