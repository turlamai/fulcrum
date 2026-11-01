#!/usr/bin/env bash
# No real Release API calls. Probe exact identity, unexpected state, and a
# PATCH which returns success without persisting the intended tag.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin"
cat > "$TMP/bin/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == api ]] || exit 80
shift
if [[ "$1" == -X ]]; then
  [[ "$2" == PATCH && "$3" == repos/turlamai/fulcrum/releases/42 ]] || exit 81
  echo PATCH >> "$MOCK_LOG"
  if [[ "$MOCK_MODE" == repair ]]; then
    jq '.tag_name = "v0.14.2"' "$MOCK_STATE" > "$MOCK_STATE.next"
    mv "$MOCK_STATE.next" "$MOCK_STATE"
  fi
else
  [[ "$1" == repos/turlamai/fulcrum/releases/42 ]] || exit 82
fi
cat "$MOCK_STATE"
MOCK
printf '#!/usr/bin/env bash\nexit 0\n' > "$TMP/bin/sleep"
chmod +x "$TMP/bin/gh" "$TMP/bin/sleep"
export PATH="$TMP/bin:$PATH"
export GH_REPO=turlamai/fulcrum RELEASE_ID=42 EXPECTED_TAG=v0.14.2
export EXPECTED_SHA=0123456789abcdef0123456789abcdef01234567
export EXPECTED_PRERELEASE=false MOCK_STATE="$TMP/state" MOCK_LOG="$TMP/log"
export MOCK_MODE=noop
state() {
  jq -n --arg tag "$1" --argjson draft "$2" --arg sha "$3" --argjson pre "$4" \
    '{id:42,tag_name:$tag,draft:$draft,target_commitish:$sha,prerelease:$pre}' > "$MOCK_STATE"
  : > "$MOCK_LOG"
}
pass() { bash "$ROOT/verify-release-draft.sh" > "$TMP/out" 2>&1 || { cat "$TMP/out"; exit 1; }; }
fail() { ! bash "$ROOT/verify-release-draft.sh" > "$TMP/out" 2>&1; }
unmodified() { [[ ! -s "$MOCK_LOG" ]]; }
state v0.14.2 true "$EXPECTED_SHA" false; pass; unmodified
state untagged-abc true "$EXPECTED_SHA" false; MOCK_MODE=repair; export MOCK_MODE; pass
[[ "$(grep -c PATCH "$MOCK_LOG")" == 1 ]]
state untagged-stuck true "$EXPECTED_SHA" false; MOCK_MODE=noop; export MOCK_MODE; fail
state v0.14.1 true "$EXPECTED_SHA" false; fail; unmodified
state untagged-other true fedcba9876543210fedcba9876543210fedcba98 false; fail; unmodified
state untagged-published false "$EXPECTED_SHA" false; fail; unmodified
state untagged-prerelease true "$EXPECTED_SHA" true; fail; unmodified
RELEASE_ID=invalid; export RELEASE_ID; fail; unmodified
echo 'release-safety: 8 draft-identity and guarded-repair tests passed'
