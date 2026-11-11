#!/usr/bin/env bash
# Authorize a version tag under the family credential contract. macOS
# releases are production-signed or fail closed (release.yml), so the five
# Apple credentials are mandatory before an immutable tag is minted.
# Windows Authenticode stays an independent opt-in mode. The Linux artifact
# key and signed update-feed key are always mandatory.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ "${RELEASE_APPROVED:-}" == true ]] || {
  echo 'release-preflight: FULCRUM_STABLE_RELEASE_APPROVED must be true; refusing immutable tag' >&2; exit 1;
}
windows="${WINDOWS_SIGNING_MODE:-unsigned}"
bash "$ROOT/scripts/check-release-signing-modes.sh" "$windows" >/dev/null
need() {
  local key
  for key in "$@"; do
    [[ -n "${!key:-}" ]] || {
      echo "release-preflight: missing mandatory $key for Windows=$windows; refusing tag" >&2
      exit 1
    }
  done
}
need DEVELOPER_ID_P12_B64 DEVELOPER_ID_P12_PASSWORD ASC_KEY_ID ASC_ISSUER_ID ASC_KEY_P8_B64 \
  LINUX_SIGN_PRIVATE_KEY LINUX_SIGN_KEY_ID RIVET_UPDATE_PRIVATE_KEY RIVET_UPDATE_KEY_ID
[[ "$RIVET_UPDATE_KEY_ID" == fulcrum-2026-10 ]] || {
  echo 'release-preflight: signed feed key ID differs from shipped client trust root' >&2; exit 1;
}
if [[ "$windows" == authenticode ]]; then
  need WINDOWS_CERTIFICATE_PFX WINDOWS_CERTIFICATE_PASSWORD WINDOWS_TIMESTAMP_URL
fi
echo "release-preflight: authorized production-signed macOS, Windows=$windows, signed feed and Linux artifact keys"
