#!/usr/bin/env python3
"""Offline regression cases for the GitHub draft-asset digest gate."""
from __future__ import annotations

import copy
import hashlib
import unittest

from check_release_public_assets import FIXED_SUFFIXES, RPM_ARCHES, UPDATE_FEEDS
from check_release_uploaded_assets import verify_uploaded_assets

VERSION = "0.14.3"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class UploadedAssetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.payloads = {
            f"fulcrum-{VERSION}-{suffix}": suffix.encode()
            for suffix in FIXED_SUFFIXES
        }
        for arch in RPM_ARCHES:
            self.payloads[f"fulcrum-{VERSION}-23-linux-{arch}.rpm"] = arch.encode()
        for name in UPDATE_FEEDS:
            self.payloads[name] = b'{"ed25519_signed":"fixture"}'
        self.checksums = "".join(
            f"{sha(data)}  {name}\n"
            for name, data in sorted(self.payloads.items())
        ).encode()
        self.release = {
            "draft": True,
            "assets": [
                {
                    "name": name,
                    "state": "uploaded",
                    "size": len(data),
                    "digest": f"sha256:{sha(data)}",
                }
                for name, data in self.payloads.items()
            ]
            + [
                {
                    "name": "SHA256SUMS",
                    "state": "uploaded",
                    "size": len(self.checksums),
                    "digest": f"sha256:{sha(self.checksums)}",
                }
            ],
        }

    def validate(self) -> None:
        verify_uploaded_assets(VERSION, self.checksums, self.release)

    def test_valid_uploaded_asset_matrix(self) -> None:
        self.assertEqual(len(self.payloads), 20)
        self.validate()

    def test_corrupt_payload_digest_is_rejected(self) -> None:
        self.release["assets"][0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(ValueError, "digest"):
            self.validate()

    def test_missing_installer_is_rejected(self) -> None:
        self.release["assets"].pop(0)
        with self.assertRaisesRegex(ValueError, "missing="):
            self.validate()

    def test_extra_versionless_alias_is_rejected(self) -> None:
        self.release["assets"].append(
            {"name": "fulcrum-linux-x64.deb", "digest": "sha256:" + "1" * 64, "size": 1, "state": "uploaded"}
        )
        with self.assertRaisesRegex(ValueError, "extra="):
            self.validate()

    def test_checksum_file_remote_digest_is_rejected(self) -> None:
        self.release["assets"][-1]["digest"] = "sha256:" + "f" * 64
        with self.assertRaisesRegex(ValueError, "SHA256SUMS"):
            self.validate()

    def test_unfinished_upload_is_rejected(self) -> None:
        self.release["assets"][0]["state"] = "starter"
        with self.assertRaisesRegex(ValueError, "not fully uploaded"):
            self.validate()

    def test_zero_size_is_rejected(self) -> None:
        self.release["assets"][0]["size"] = 0
        with self.assertRaisesRegex(ValueError, "empty"):
            self.validate()

    def test_non_draft_is_rejected(self) -> None:
        self.release["draft"] = False
        with self.assertRaisesRegex(ValueError, "non-draft"):
            self.validate()

    def test_missing_checksum_is_rejected(self) -> None:
        self.checksums = b"\n".join(self.checksums.splitlines()[1:]) + b"\n"
        with self.assertRaisesRegex(ValueError, "missing="):
            self.validate()

    def test_duplicate_checksum_is_rejected(self) -> None:
        self.checksums += self.checksums.splitlines(keepends=True)[0]
        with self.assertRaisesRegex(ValueError, "Duplicate checksum"):
            self.validate()

    def test_mismatched_signed_feed_digests_rejected(self) -> None:
        lines = self.checksums.decode().splitlines(keepends=True)
        for i, line in enumerate(lines):
            if line.endswith("  manifest.json\n"):
                lines[i] = ("a" * 64) + "  manifest.json\n"
        self.checksums = "".join(lines).encode()
        with self.assertRaisesRegex(ValueError, "signed updater digests"):
            self.validate()

    def test_duplicate_rpm_revision_is_rejected(self) -> None:
        name = f"fulcrum-{VERSION}-24-linux-aarch64.rpm"
        line = f"{sha(b'another revision')}  {name}\n"
        lines = self.checksums.decode().splitlines(keepends=True) + [line]
        self.checksums = "".join(sorted(lines, key=lambda x: x.split("  ", 1)[1])).encode()
        with self.assertRaisesRegex(ValueError, "one RPM revision"):
            self.validate()


if __name__ == "__main__":
    unittest.main()
