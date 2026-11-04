#!/usr/bin/env python3
"""Offline regression tests for the exact public Fulcrum release inventory."""
import hashlib
import tempfile
import unittest
from pathlib import Path

from check_release_public_assets import (
    FIXED_SUFFIXES,
    RPM_ARCHES,
    UPDATE_FEEDS,
    verify_inventory,
    write_checksums,
)

VERSION = "0.14.3"


class ReleaseInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.public = Path(self.temp.name)
        for suffix in FIXED_SUFFIXES:
            name = f"fulcrum-{VERSION}-{suffix}"
            (self.public / name).write_bytes(name.encode("ascii"))
        for arch in RPM_ARCHES:
            name = f"fulcrum-{VERSION}-23-linux-{arch}.rpm"
            (self.public / name).write_bytes(name.encode("ascii"))
        for feed in UPDATE_FEEDS:
            (self.public / feed).write_bytes(b'{"signed":"fixture"}\n')

    def test_complete_release_includes_both_signed_feeds_in_checksums(self) -> None:
        manifest = write_checksums(self.public, VERSION)
        rows = manifest.read_text().splitlines()
        self.assertEqual(len(rows), 20)
        actual = {}
        for row in rows:
            digest, name = row.split("  ", 1)
            self.assertNotIn(name, actual)
            actual[name] = digest
            self.assertEqual(
                digest,
                hashlib.sha256((self.public / name).read_bytes()).hexdigest(),
            )
        self.assertEqual(list(actual), sorted(actual))
        for feed in UPDATE_FEEDS:
            self.assertIn(feed, actual)

    def test_missing_installer_fails_closed(self) -> None:
        (self.public / f"fulcrum-{VERSION}-windows-x64.msi").unlink()
        with self.assertRaisesRegex(ValueError, "missing="):
            write_checksums(self.public, VERSION)

    def test_duplicate_rpm_revision_fails_closed(self) -> None:
        (self.public / f"fulcrum-{VERSION}-24-linux-x86_64.rpm").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "exactly one versioned RPM"):
            write_checksums(self.public, VERSION)

    def test_missing_rpm_architecture_fails_closed(self) -> None:
        (self.public / f"fulcrum-{VERSION}-23-linux-aarch64.rpm").unlink()
        with self.assertRaisesRegex(ValueError, "exactly one versioned RPM"):
            write_checksums(self.public, VERSION)

    def test_duplicate_alias_and_sidecar_are_rejected(self) -> None:
        for extra in ("fulcrum-linux-arm64.deb", f"fulcrum-{VERSION}-macos-arm64.dmg.sha256"):
            path = self.public / extra
            path.write_bytes(b"duplicate")
            with self.assertRaisesRegex(ValueError, "extra="):
                verify_inventory(self.public, VERSION)
            path.unlink()

    def test_divergent_legacy_feed_fails_closed(self) -> None:
        (self.public / "manifest.json").write_bytes(b'{"tampered":true}\n')
        with self.assertRaisesRegex(ValueError, "signed update feeds differ"):
            write_checksums(self.public, VERSION)

    def test_empty_asset_fails_closed(self) -> None:
        (self.public / f"fulcrum-{VERSION}-linux-amd64.deb").write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "Empty, invalid"):
            write_checksums(self.public, VERSION)

    def test_invalid_version_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "SemVer"):
            write_checksums(self.public, "../0.14.3")


if __name__ == "__main__":
    unittest.main()
