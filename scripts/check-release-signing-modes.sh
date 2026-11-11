#!/usr/bin/env bash
# Independent Windows trust policy. macOS production signing is
# credential-driven (family 5 secrets: DEVELOPER_ID_P12_B64,
# DEVELOPER_ID_P12_PASSWORD, ASC_KEY_ID, ASC_ISSUER_ID, ASC_KEY_P8_B64)
# and has no repository-variable mode switch. Empty vars are safe, explicit
# defaults. Invalid values fail closed before any release packaging begins.
set -euo pipefail
fail() { echo "release-signing-modes: $*" >&2; exit 1; }
validate() {
  local windows="${1:-unsigned}"
  case "$windows" in unsigned|authenticode) ;; *) fail "invalid Windows mode '$windows' (unsigned|authenticode)" ;; esac
  printf 'release-signing-modes: Windows=%s\n' "$windows"
}
if [[ "${1:-}" == "--self-test" ]]; then
  for win in "" unsigned authenticode; do validate "$win" >/dev/null; done
  for win in signed adhoc developer-id invalid; do
    if (validate "$win" >/dev/null 2>&1); then fail "accepted invalid Windows mode"; fi
  done
  echo "release-signing-modes: all valid and invalid combinations passed"
else
  validate "${1:-}"
fi
