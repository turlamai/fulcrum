#!/usr/bin/env bash
# CI-only static release contract: independent signing, Ed25519 update trust,
# and explicit stable promotion. PR runners need no publisher credentials.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release="$ROOT/.github/workflows/release.yml"
require() {
  grep -Fq -- "$1" "$release" || {
    echo "release-safety: missing required gate: $1" >&2
    exit 1
  }
}
bash "$ROOT/scripts/check-release-signing-modes.sh" --self-test
require 'Validate independent platform signing modes'
require 'scripts/check-release-signing-modes.sh "$MACOS_SIGNING_MODE" "$WINDOWS_SIGNING_MODE"'
require "if: matrix.platform == 'macos' && vars.FULCRUM_MACOS_SIGNING_MODE == 'developer-id'"
require "if: matrix.platform == 'windows' && vars.FULCRUM_WINDOWS_SIGNING_MODE == 'authenticode'"
require "matrix.platform == 'linux' || (matrix.platform == 'macos' && vars.FULCRUM_MACOS_SIGNING_MODE == 'developer-id')"
require "matrix.platform == 'windows' && vars.FULCRUM_WINDOWS_SIGNING_MODE == 'authenticode'"
require "matrix.platform == 'macos' && vars.FULCRUM_MACOS_SIGNING_MODE != 'developer-id'"
require "matrix.platform == 'windows' && vars.FULCRUM_WINDOWS_SIGNING_MODE != 'authenticode'"
require 'raco rivet release --development'
require 'raco rivet release'
require 'raco rivet verify --production'
require 'xcrun notarytool store-credentials'
require 'xcrun stapler validate'
require 'spctl --assess'
require 'WINDOWS_CERTIFICATE_PFX'
require 'RIVET_WINDOWS_TIMESTAMP_URL'
require 'RIVET_LINUX_SIGN_PRIVATE_KEY'
require '.tar.gz.sig'
require 'RIVET_UPDATE_PRIVATE_KEY_PEM'
require 'RIVET_UPDATE_KEY_ID'
require 'FULCRUM_STABLE_RELEASE_APPROVED'
require 'Disclose platform-specific trust before publishing'
require 'macOS ad-hoc signed, NOT notarized'
require 'Windows MSI/ZIP unsigned'
require 'Resolve draft release REST id'
require 'releases?per_page=100'
require 'RELEASE_ID: ${{ steps.release.outputs.id }}'
require '-f tag_name="$tag" -f target_commitish="$GITHUB_SHA"'
require 'needs: github-release'
require 'Verify installers and signed feed exist before publishing'
require 'Require green CI and Product Acceptance for tagged commit'
require 'head_sha=$GITHUB_SHA'
require 'actions: read'
if grep -Fq 'FULCRUM_UNSIGNED_RELEASE_APPROVED' "$release"; then
  echo 'release-safety: legacy global unsigned switch must not control signing' >&2
  exit 1
fi
[[ "$(grep -c 'raco rivet release --development' "$release")" == 1 ]] || {
  echo 'release-safety: expected one gated development release step' >&2; exit 1;
}
banner_line="$(grep -n -m1 'name: Disclose platform-specific trust before publishing' "$release" | cut -d: -f1)"
publish_line="$(grep -n -m1 'name: Verify installers and signed feed exist before publishing' "$release" | cut -d: -f1)"
[[ "$banner_line" -lt "$publish_line" ]] || {
  echo 'release-safety: trust disclosure must precede promotion' >&2; exit 1;
}
echo 'release-safety: independent signing, promotion and signed updates checked'
