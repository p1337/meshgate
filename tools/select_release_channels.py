#!/usr/bin/env python3
"""Select the stable and candidate MeshGate releases using SemVer precedence."""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from functools import cmp_to_key
from pathlib import Path


SEMVER = re.compile(
    r"^[vV](0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


@dataclass(frozen=True)
class Version:
    tag: str
    core: tuple[int, int, int]
    prerelease: tuple[str, ...]


def parse(tag: str) -> Version | None:
    match = SEMVER.fullmatch(tag)
    if not match:
        return None
    prerelease = tuple(match.group(4).split(".")) if match.group(4) else ()
    if any(part.isdigit() and len(part) > 1 and part.startswith("0") for part in prerelease):
        return None
    return Version(tag, tuple(map(int, match.group(1, 2, 3))), prerelease)


def compare(left: Version, right: Version) -> int:
    if left.core != right.core:
        return (left.core > right.core) - (left.core < right.core)
    if not left.prerelease or not right.prerelease:
        return (not left.prerelease) - (not right.prerelease)
    for left_part, right_part in zip(left.prerelease, right.prerelease):
        if left_part == right_part:
            continue
        left_numeric = left_part.isdigit()
        right_numeric = right_part.isdigit()
        if left_numeric and right_numeric:
            return (int(left_part) > int(right_part)) - (int(left_part) < int(right_part))
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return (left_part > right_part) - (left_part < right_part)
    return (len(left.prerelease) > len(right.prerelease)) - (
        len(left.prerelease) < len(right.prerelease)
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: select_release_channels.py RELEASES.json")
    source = sys.stdin.read() if sys.argv[1] == "-" else Path(sys.argv[1]).read_text(encoding="utf-8")
    releases = json.loads(source)
    versions = []
    for release in releases:
        if release.get("draft"):
            continue
        version = parse(str(release.get("tag_name", "")))
        if version is not None:
            versions.append(version)
    if not versions:
        raise SystemExit("no valid published MeshGate SemVer releases found")

    candidate = max(versions, key=cmp_to_key(compare))
    stable_versions = [version for version in versions if not version.prerelease]
    if not stable_versions:
        raise SystemExit("no stable MeshGate release found")
    stable = max(stable_versions, key=cmp_to_key(compare))
    print(f"stable_tag={stable.tag}")
    print(f"candidate_tag={candidate.tag}")


if __name__ == "__main__":
    main()
