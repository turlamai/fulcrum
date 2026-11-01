#!/usr/bin/env bash
# Offline regression for the Release REST ID handoff gates.
# No API requests, release tokens, tags or publication side effects.
set -euo pipefail

command -v jq >/dev/null || { echo 'jq required for release identity tests' >&2; exit 1; }

tag='v0.14.2'
prerelease=false
valid='{"id":430000001,"tag_name":"v0.14.2","draft":true,"prerelease":false}'
bad_tag='{"id":430000001,"tag_name":"v0.14.1","draft":true,"prerelease":false}'
untagged='{"id":430000001,"tag_name":"untagged-aabbccddee","draft":true,"prerelease":false}'
published='{"id":430000001,"tag_name":"v0.14.2","draft":false,"prerelease":false}'
wrong_channel='{"id":430000001,"tag_name":"v0.14.2","draft":true,"prerelease":true}'
expression='(.tag_name == $tag) and (.draft == true) and (.prerelease == $prerelease)'

accepts() {
  printf '%s\n' "$1" | jq -e --arg tag "$tag" \
    --argjson prerelease "$prerelease" "$expression" >/dev/null 2>&1
}
rejects() {
  if accepts "$1"; then
    echo "release identity test: unexpectedly accepted $2" >&2
    exit 1
  fi
}

accepts "$valid" || { echo 'release identity test: valid draft rejected' >&2; exit 1; }
rejects "$bad_tag" 'wrong tag'
rejects "$untagged" 'GitHub untagged placeholder'
rejects "$published" 'prematurely published release'
rejects "$wrong_channel" 'incorrect prerelease channel'
rejects '{"id":430000001,"draft":true,"prerelease":false}' 'missing tag'
rejects '{"id":430000001,"tag_name":"v0.14.2","draft":true}' 'missing prerelease state'
rejects 'not-json' 'malformed REST response'

# The publication confirmation is a distinct state; it must refuse
# remaining drafts, including a PATCH that reported success but did nothing.
publication='(.tag_name == $tag) and (.draft == false) and (.prerelease == $prerelease)'
printf '%s\n' "$published" | jq -e --arg tag "$tag" --argjson prerelease "$prerelease" \
  "$publication" >/dev/null
if printf '%s\n' "$valid" | jq -e --arg tag "$tag" --argjson prerelease "$prerelease" \
  "$publication" >/dev/null 2>&1; then
  echo 'release identity test: no-op publish PATCH was accepted' >&2
  exit 1
fi
echo 'release identity: offline draft and publish guards passed'
