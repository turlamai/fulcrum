#!/usr/bin/env bash
# Offline regression for the Release REST ID handoff gates.
# No API requests, release tokens, tags or publication side effects.
set -euo pipefail

command -v jq >/dev/null || { echo 'jq required for release identity tests' >&2; exit 1; }

tag='v0.14.2'
prerelease=false
sha='0123456789abcdef0123456789abcdef01234567'
release_id=430000001
valid='{"id":430000001,"tag_name":"v0.14.2","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":false}'
bad_tag='{"id":430000001,"tag_name":"v0.14.1","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":false}'
untagged='{"id":430000001,"tag_name":"untagged-aabbccddee","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":false}'
published='{"id":430000001,"tag_name":"v0.14.2","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":false,"prerelease":false}'
wrong_channel='{"id":430000001,"tag_name":"v0.14.2","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":true}'
# Draft stage: GitHub detaches the tag name of a draft referencing an
# existing ref into an untagged-<hash> placeholder until publication (#146),
# so the draft predicate accepts both shapes; every other conjunct stays
# strict and anchored on the numeric REST id.
expression='((.tag_name == $tag) or (.tag_name | startswith("untagged-"))) and (.draft == true) and (.prerelease == $prerelease) and (.target_commitish == $sha) and (.id == $id)'

accepts() {
  printf '%s\n' "$1" | jq -e --arg tag "$tag" \
    --arg sha "$sha" --argjson id "$release_id" \
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
rejects '{"id":430000002,"tag_name":"v0.14.2","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":false}' 'foreign release id'
rejects '{"id":430000001,"tag_name":"v0.14.2","target_commitish":"fedcba9876543210fedcba9876543210fedcba98","draft":true,"prerelease":false}' 'wrong commit'
accepts "$untagged" || { echo 'release identity test: untagged draft placeholder rejected — GitHub detaches the tag name of a draft referencing an existing ref (#146)' >&2; exit 1; }
rejects "$published" 'prematurely published release'
rejects "$wrong_channel" 'incorrect prerelease channel'
rejects '{"id":430000001,"target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true,"prerelease":false}' 'missing tag'
rejects '{"id":430000001,"tag_name":"v0.14.2","target_commitish":"0123456789abcdef0123456789abcdef01234567","draft":true}' 'missing prerelease state'
rejects 'not-json' 'malformed REST response'

# The publication confirmation is a distinct state; it must refuse
# remaining drafts, including a PATCH that reported success but did nothing.
publication='(.tag_name == $tag) and (.draft == false) and (.prerelease == $prerelease) and (.target_commitish == $sha) and (.id == $id)'
printf '%s\n' "$published" | jq -e --arg tag "$tag" --arg sha "$sha" --argjson id "$release_id" \
  --argjson prerelease "$prerelease" "$publication" >/dev/null
if printf '%s\n' "$valid" | jq -e --arg tag "$tag" --arg sha "$sha" --argjson id "$release_id" \
  --argjson prerelease "$prerelease" "$publication" >/dev/null 2>&1; then
  echo 'release identity test: no-op publish PATCH was accepted' >&2
  exit 1
fi
# Publication is where the tag binding becomes real and checkable: the
# untagged placeholder must never satisfy the post-publish predicate.
if printf '%s\n' "$untagged" | jq -e --arg tag "$tag" --arg sha "$sha" --argjson id "$release_id" \
  --argjson prerelease "$prerelease" "$publication" >/dev/null 2>&1; then
  echo 'release identity test: untagged placeholder accepted at publication' >&2
  exit 1
fi
echo 'release identity: offline draft and publish guards passed'
