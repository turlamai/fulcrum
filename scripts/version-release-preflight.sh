#!/usr/bin/env bash
# Authorize a version tag using independent macOS/Windows signing modes.
# The Linux artifact key and signed update-feed key are always mandatory.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ "${RELEASE_APPROVED:-}" == true ]] || {
  echo 'release-preflight: FULCRUM_STABLE_RELEASE_APPROVED must be true; refusing immutable tag' >&2; exit 1;
}
macos="${MACOS_SIGNING_MODE:-adhoc}"
windows="${WINDOWS_SIGNING_MODE:-unsigned}"
bash "$ROOT/scripts/check-release-signing-modes.sh" "$macos" "$windows" >/dev/null
need() {
  local key
  for key in "$@"; do
    [[ -n "${!key:-}" ]] || {
      echo "release-preflight: missing mandatory $key for macOS=$macos Windows=$windows; refusing tag" >&2
      exit 1
    }
  done
}
need LINUX_SIGN_PRIVATE_KEY LINUX_SIGN_KEY_ID RIVET_UPDATE_PRIVATE_KEY RIVET_UPDATE_KEY_ID
[[ "$RIVET_UPDATE_KEY_ID" == fulcrum-2026-10 ]] || {
  echo 'release-preflight: signed feed key ID differs from shipped client trust root' >&2; exit 1;
}
if [[ "$macos" == developer-id ]]; then
  need MACOS_CERTIFICATE_P12 MACOS_CERTIFICATE_PASSWORD MACOS_SIGN_IDENTITY \
    APPLE_ID APPLE_TEAM_ID MACOS_APP_SPECIFIC_PASSWORD
fi
if [[ "$windows" == authenticode ]]; then
  need WINDOWS_CERTIFICATE_PFX WINDOWS_CERTIFICATE_PASSWORD WINDOWS_TIMESTAMP_URL
fi
echo "release-preflight: authorized macOS=$macos Windows=$windows with signed feed and Linux artifact keys"
