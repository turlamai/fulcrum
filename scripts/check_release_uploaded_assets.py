#!/usr/bin/env python3
"""Verify uploaded GitHub Release assets before promoting a Fulcrum draft.

GitHub computes each asset's SHA-256 digest independently. Compare those
digests with the exact inventory generated and checked in the draft job;
file names or an uploaded SHA256SUMS alone do not prove byte integrity.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

from check_release_public_assets import FIXED_SUFFIXES, RPM_ARCHES, UPDATE_FEEDS, VERSION_PATTERN

CHECKSUM_LINE = re.compile(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._-]*)")


def verify_uploaded_assets(version: str, checksums: bytes, release: dict) -> None:
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"Invalid release version {version!r}")
    if release.get("draft") is not True:
        raise ValueError("Refusing to verify/promote a non-draft release")

    expected = {f"fulcrum-{version}-{suffix}" for suffix in FIXED_SUFFIXES}
    expected.update(UPDATE_FEEDS)

    rows: dict[str, str] = {}
    names: list[str] = []
    try:
        lines = checksums.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ValueError("SHA256SUMS is not UTF-8") from exc
    for line in lines:
        match = CHECKSUM_LINE.fullmatch(line)
        if not match:
            raise ValueError(f"Malformed SHA256SUMS entry: {line[:100]!r}")
        digest, name = match.groups()
        if name in rows:
            raise ValueError(f"Duplicate checksum entry: {name}")
        rows[name] = digest
        names.append(name)
    if names != sorted(names):
        raise ValueError("SHA256SUMS must be sorted by filename")

    for arch in RPM_ARCHES:
        pattern = re.compile(
            rf"fulcrum-{re.escape(version)}-[0-9]+-linux-{arch}\.rpm"
        )
        matches = [name for name in rows if pattern.fullmatch(name)]
        if len(matches) != 1:
            raise ValueError(f"Expected one RPM revision for {arch}, got {matches}")
        expected.add(matches[0])

    if set(rows) != expected or len(rows) != 20:
        raise ValueError(
            "Uploaded checksum inventory mismatch: "
            f"missing={sorted(expected - set(rows))}, "
            f"extra={sorted(set(rows) - expected)}"
        )
    if rows[UPDATE_FEEDS[0]] != rows[UPDATE_FEEDS[1]]:
        raise ValueError("Legacy and canonical signed updater digests differ")

    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ValueError("Draft release did not expose an asset list")
    uploaded: dict[str, dict] = {}
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
            raise ValueError("Malformed GitHub Release asset")
        name = asset["name"]
        if name in uploaded:
            raise ValueError(f"Duplicate GitHub Release asset: {name}")
        uploaded[name] = asset

    expected_remote = expected | {"SHA256SUMS"}
    if set(uploaded) != expected_remote:
        raise ValueError(
            "Draft release assets mismatch: "
            f"missing={sorted(expected_remote - set(uploaded))}, "
            f"extra={sorted(set(uploaded) - expected_remote)}"
        )

    expected_digests = dict(rows)
    expected_digests["SHA256SUMS"] = hashlib.sha256(checksums).hexdigest()
    for name in sorted(expected_remote):
        asset = uploaded[name]
        if asset.get("state") != "uploaded":
            raise ValueError(f"GitHub asset is not fully uploaded: {name}")
        size = asset.get("size")
        if type(size) is not int or size <= 0:
            raise ValueError(f"GitHub asset is empty or lacks a valid size: {name}")
        if asset.get("digest") != f"sha256:{expected_digests[name]}":
            raise ValueError(f"GitHub asset digest does not match staged bytes: {name}")


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: check_release_uploaded_assets.py VERSION SHA256SUMS < release.json",
            file=sys.stderr,
        )
        return 2
    try:
        checksums = Path(sys.argv[2]).read_bytes()
        release = json.load(sys.stdin)
        verify_uploaded_assets(sys.argv[1], checksums, release)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        print(f"release-upload: {exc}", file=sys.stderr)
        return 1
    print("release-upload: 18 payloads + 2 signed feeds + SHA256SUMS match GitHub digests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
