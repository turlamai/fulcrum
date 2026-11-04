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
bash "$ROOT/scripts/test-version-release-preflight.sh"
grep -Fq 'run: bash scripts/version-release-preflight.sh' "$ROOT/.github/workflows/version-release.yml" || {
  echo 'release-safety: VERSION tag gate must use independent-mode preflight' >&2
  exit 1
}
bash "$ROOT/scripts/test-release-draft-identity.sh"
python3 "$ROOT/scripts/test_release_public_assets.py"
python3 "$ROOT/scripts/test_release_uploaded_assets.py"
require 'python3 scripts/check_release_public_assets.py "$version" public'
require 'Retain canonical checksums for draft promotion'
require 'fulcrum-public-checksums'
require 'python3 scripts/check_release_uploaded_assets.py "$version" release-verify/SHA256SUMS'
require 'cp release/update-manifest.json release/manifest.json public/'
require '(cd public && sha256sum -c SHA256SUMS)'
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
require 'release_id: ${{ steps.create-release.outputs.id }}'
require 'id: create-release'
require 'tag_name: ${{ github.ref_name }}'
require 'target_commitish: ${{ github.sha }}'
require 'Confirm exact draft identity before pruning stale assets'
require 'RELEASE_ID: ${{ steps.create-release.outputs.id }}'
require 'RELEASE_ID: ${{ needs.github-release.outputs.release_id }}'
require '(.target_commitish == $sha)'
require '(.id == $id)'
require 'argjson id "$RELEASE_ID" --arg sha "$GITHUB_SHA"'
require 'release_id="${RELEASE_ID}"'
require 'RELEASE_ID: ${{ steps.release.outputs.id }}'
if grep -Fq 'releases?per_page=100' "$release"; then
  echo 'release-safety: do not rediscover drafts through a non-unique list' >&2
  exit 1
fi
# Drafts are not reliably addressable by tag through gh release view.
# The release must be checked by numeric REST id before and after publication.
require 'release_json="$(gh api "repos/$GH_REPO/releases/$RELEASE_ID")"'
require 'published_json="$(gh api "repos/$GH_REPO/releases/$RELEASE_ID")"'
# Drafts referencing an existing tag carry GitHub's untagged-<hash>
# placeholder until publication (#146): all THREE draft-stage identity
# gates (creator, REST id resolver, publisher) must tolerate the detached
# shape while remaining strict on REST id, SHA and prerelease channel.
# The post-publish confirmation must still demand the exact tag name.
[[ "$(grep -Fc '((.tag_name == $tag) or (.tag_name | startswith("untagged-"))) and (.draft == true) and (.prerelease == $prerelease)' "$release")" == 3 ]] || {
  echo 'release-safety: expected anomaly-tolerant draft identity gates in creator, resolver and publisher' >&2
  exit 1
}
require '(.tag_name == $tag) and (.draft == false) and (.prerelease == $prerelease)'
# Rerun cleanup must never delete an existing asset until the draft's
# REST identity (numeric id, tag, SHA, channel) has been verified.
draft_gate_line="$(grep -n -m1 'name: Confirm exact draft identity before pruning stale assets' "$release" | cut -d: -f1)"
prune_step_line="$(grep -n -m1 'name: Prune stale or build-only draft assets' "$release" | cut -d: -f1)"
[[ -n "$draft_gate_line" && -n "$prune_step_line" && "$draft_gate_line" -lt "$prune_step_line" ]] || {
  echo 'release-safety: draft identity gate must precede cleanup of stale assets' >&2
  exit 1
}
if grep -Fq 'gh release view "$tag" --json isDraft' "$release"; then
  echo 'release-safety: do not query drafts by tag' >&2
  exit 1
fi
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
