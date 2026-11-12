#!/usr/bin/env bash
# Offline credential-matrix tests; fake values, no API calls or tags.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
preflight="$ROOT/scripts/version-release-preflight.sh"
export RELEASE_APPROVED=true
# Mandatory family Apple production credentials (tag releases fail closed
# without them, so the "unsigned default" only covers Windows now).
export DEVELOPER_ID_P12_B64=fake-p12 DEVELOPER_ID_P12_PASSWORD=fake-pass
export ASC_KEY_ID=fake-key ASC_ISSUER_ID=fake-issuer ASC_KEY_P8_B64=fake-p8
export LINUX_SIGN_PRIVATE_KEY=fake-linux LINUX_SIGN_KEY_ID=fake-id
export RIVET_UPDATE_PRIVATE_KEY=fake-feed RIVET_UPDATE_KEY_ID=fulcrum-2026-10
export WINDOWS_CERTIFICATE_PFX=fake-pfx WINDOWS_CERTIFICATE_PASSWORD=fake-pass
export WINDOWS_TIMESTAMP_URL=https://timestamp.invalid
pass() { bash "$preflight" >/dev/null 2>&1 || { echo "release-preflight: expected success: $1" >&2; exit 1; }; }
fail() { if bash "$preflight" >/dev/null 2>&1; then echo "release-preflight: unexpectedly accepted $1" >&2; exit 1; fi; }
for win in unsigned authenticode; do
  WINDOWS_SIGNING_MODE="$win" pass "windows-$win"
done
WINDOWS_SIGNING_MODE='' WINDOWS_TIMESTAMP_URL='' \
  WINDOWS_CERTIFICATE_PFX='' WINDOWS_CERTIFICATE_PASSWORD='' pass default-windows-unsigned
RELEASE_APPROVED=false fail missing-approval
WINDOWS_SIGNING_MODE=invalid fail invalid-windows
RIVET_UPDATE_KEY_ID=wrong fail wrong-key-id
RIVET_UPDATE_PRIVATE_KEY='' fail missing-feed-key
LINUX_SIGN_PRIVATE_KEY='' fail missing-linux-key
DEVELOPER_ID_P12_B64='' DEVELOPER_ID_P12_PASSWORD='' fail missing-developer-id-p12
ASC_KEY_P8_B64='' fail missing-asc-key
ASC_KEY_ID='' ASC_ISSUER_ID='' fail missing-asc-key-ids
WINDOWS_SIGNING_MODE=authenticode WINDOWS_CERTIFICATE_PFX='' fail missing-windows-certificate
WINDOWS_SIGNING_MODE=authenticode WINDOWS_TIMESTAMP_URL='' fail missing-timestamp
echo 'release-preflight: windows modes, mandatory Apple production credentials and refusal tests passed'
