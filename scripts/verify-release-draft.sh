#!/usr/bin/env bash
# Check the exact GitHub Release returned by the draft-creation action.
# Repair ONLY that untagged draft when release id, SHA, and mode match.
set -euo pipefail
: "$GH_REPO" "$RELEASE_ID" "$EXPECTED_TAG" "$EXPECTED_SHA" "$EXPECTED_PRERELEASE"
[[ "$RELEASE_ID" =~ ^[0-9]+$ ]] || { echo 'release-safety: invalid release ID' >&2; exit 1; }
[[ "$EXPECTED_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$ ]] || { echo 'release-safety: invalid tag' >&2; exit 1; }
[[ "$EXPECTED_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo 'release-safety: invalid commit' >&2; exit 1; }
[[ "$EXPECTED_PRERELEASE" == true || "$EXPECTED_PRERELEASE" == false ]] || exit 1
endpoint="repos/$GH_REPO/releases/$RELEASE_ID"
read_release() { gh api "$endpoint"; }
accept() {
  jq -e --argjson id "$RELEASE_ID" --arg tag "$EXPECTED_TAG" \
    --arg sha "$EXPECTED_SHA" --argjson pre "$EXPECTED_PRERELEASE" \
    '(.id == $id and .tag_name == $tag and .target_commitish == $sha
      and .draft == true and .prerelease == $pre)' <<< "$1" >/dev/null
}
actual="$(read_release)"
if accept "$actual"; then echo 'release-safety: exact tagged draft verified'; exit 0; fi
# An unexpected known tag, another SHA, or an already published release is
# NEVER editable by this recovery path.
if ! jq -e --argjson id "$RELEASE_ID" --arg sha "$EXPECTED_SHA" \
  --argjson pre "$EXPECTED_PRERELEASE" \
  '(.id == $id and .draft == true and .prerelease == $pre
    and .target_commitish == $sha and (.tag_name | startswith("untagged-")))' \
  <<< "$actual" >/dev/null; then
  echo 'release-safety: foreign draft or published release, refusing mutation' >&2
  jq '{id,tag_name,draft,prerelease,target_commitish}' <<< "$actual" >&2
  exit 1
fi
gh api -X PATCH "$endpoint" -f tag_name="$EXPECTED_TAG" \
  -f target_commitish="$EXPECTED_SHA" -f name="$EXPECTED_TAG" \
  -F draft=true -F prerelease="$EXPECTED_PRERELEASE" >/dev/null
for attempt in 1 2 3 4; do
  actual="$(read_release)"
  if accept "$actual"; then
    echo "release-safety: repaired and re-read draft on attempt $attempt"
    exit 0
  fi
  [[ "$attempt" == 4 ]] || sleep 2
done
echo 'release-safety: tag repair did not persist; blocking aliases and promotion' >&2
jq '{id,tag_name,draft,prerelease,target_commitish}' <<< "$actual" >&2
exit 1
