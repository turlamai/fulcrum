#!/usr/bin/env bash
# Offline credential-matrix tests; fake values, no API calls or tags.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
preflight="$ROOT/scripts/version-release-preflight.sh"
export RELEASE_APPROVED=true
export LINUX_SIGN_PRIVATE_KEY=fake-linux LINUX_SIGN_KEY_ID=fake-id
export RIVET_UPDATE_PRIVATE_KEY=fake-feed RIVET_UPDATE_KEY_ID=fulcrum-2026-10
export MACOS_CERTIFICATE_P12=fake-p12 MACOS_CERTIFICATE_PASSWORD=fake-pass
export MACOS_SIGN_IDENTITY=fake-identity APPLE_ID=fake-apple APPLE_TEAM_ID=fake-team
export MACOS_APP_SPECIFIC_PASSWORD=fake-notary
export WINDOWS_CERTIFICATE_PFX=fake-pfx WINDOWS_CERTIFICATE_PASSWORD=fake-pass
export WINDOWS_TIMESTAMP_URL=https://timestamp.invalid
pass() { bash "$preflight" >/dev/null 2>&1 || { echo "release-preflight: expected success: $1" >&2; exit 1; }; }
fail() { if bash "$preflight" >/dev/null 2>&1; then echo "release-preflight: unexpectedly accepted $1" >&2; exit 1; fi; }
for mac in adhoc developer-id; do
  for win in unsigned authenticode; do
    MACOS_SIGNING_MODE="$mac" WINDOWS_SIGNING_MODE="$win" pass "$mac/$win"
  done
done
MACOS_SIGNING_MODE='' WINDOWS_SIGNING_MODE='' WINDOWS_TIMESTAMP_URL='' \
  WINDOWS_CERTIFICATE_PFX='' MACOS_CERTIFICATE_P12='' pass default-unsigned
RELEASE_APPROVED=false fail missing-approval
MACOS_SIGNING_MODE=invalid fail invalid-macos
WINDOWS_SIGNING_MODE=invalid fail invalid-windows
RIVET_UPDATE_KEY_ID=wrong fail wrong-key-id
RIVET_UPDATE_PRIVATE_KEY='' fail missing-feed-key
LINUX_SIGN_PRIVATE_KEY='' fail missing-linux-key
MACOS_SIGNING_MODE=developer-id MACOS_CERTIFICATE_P12='' fail missing-macos-certificate
WINDOWS_SIGNING_MODE=authenticode WINDOWS_CERTIFICATE_PFX='' fail missing-windows-certificate
WINDOWS_SIGNING_MODE=authenticode WINDOWS_TIMESTAMP_URL='' fail missing-timestamp
echo 'release-preflight: 4 mode combinations, unsigned defaults and 9 refusal tests passed'
