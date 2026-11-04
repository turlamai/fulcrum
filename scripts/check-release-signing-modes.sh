#!/usr/bin/env bash
# Independent platform trust policy. Empty vars are safe, explicit defaults.
# Invalid values fail closed before any release packaging begins.
set -euo pipefail
fail() { echo "release-signing-modes: $*" >&2; exit 1; }
validate() {
  local macos="${1:-adhoc}" windows="${2:-unsigned}"
  case "$macos" in adhoc|developer-id) ;; *) fail "invalid macOS mode '$macos' (adhoc|developer-id)" ;; esac
  case "$windows" in unsigned|authenticode) ;; *) fail "invalid Windows mode '$windows' (unsigned|authenticode)" ;; esac
  printf 'release-signing-modes: macOS=%s Windows=%s\n' "$macos" "$windows"
}
if [[ "${1:-}" == "--self-test" ]]; then
  for mac in "" adhoc developer-id; do
    for win in "" unsigned authenticode; do validate "$mac" "$win" >/dev/null; done
  done
  for mac in invalid production notarized; do
    if (validate "$mac" unsigned >/dev/null 2>&1); then fail "accepted invalid macOS mode"; fi
  done
  for win in signed developer-id invalid; do
    if (validate adhoc "$win" >/dev/null 2>&1); then fail "accepted invalid Windows mode"; fi
  done
  echo "release-signing-modes: all valid and invalid combinations passed"
else
  validate "${1:-}" "${2:-}"
fi
