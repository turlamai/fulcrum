# Release runbook

How a Fulcrum release ships: version bump, update keys, signing, tagged
pipeline. This is the operator document; `docs/release-and-updates.md` in
the Rivet repository explains the trust model this builds on.

## 0. Prerequisites (one-time per machine)

- Racket CS (the version Fulcrum embeds), on PATH as `raco`/`racket`.
- macOS: Xcode command line tools. The five family production secrets
  (`DEVELOPER_ID_P12_B64`, `DEVELOPER_ID_P12_PASSWORD`, `ASC_KEY_ID`,
  `ASC_ISSUER_ID`, `ASC_KEY_P8_B64`) are mandatory for tag releases — tag
  pushes without them fail closed. Manual dispatch builds without the
  secrets stay ad-hoc and are not notarized; Gatekeeper may warn.
- Windows: Visual Studio Build Tools (Desktop C++ + WinUI workload) and WiX 5.
  An Authenticode certificate and timestamp URL are required **only** when
  `FULCRUM_WINDOWS_SIGNING_MODE=authenticode`. Default `unsigned` installs
  may show a SmartScreen warning; no paid Windows developer account is needed.
- An Ed25519 update key pair, generated and stored **outside** any checkout:

  ```bash
  openssl genpkey -algorithm Ed25519 -outform DER -out update-private.der
  openssl pkey -inform DER -in update-private.der -pubout -outform DER -out update-public.der
  ```

## 1. Version bump

Release identity lives in two files that must agree: bump `version` (SemVer)
and `build` (integer, never reuse per platform) in `rivet.rktd`, and set the
repo-root `VERSION` file to the same value. Update `CHANGELOG.md` with a
section named exactly after the new version — the tag validator rejects a
tag whose version has no matching changelog section.
`scripts/check-release-version.sh` (run in CI and every release packaging
job) enforces `VERSION == rivet.rktd`; a stable tag is `v<VERSION>`, while a
candidate tag may be `v<VERSION>-rc.N`. `.github/sign-manifest.rkt` reads the
numeric package version from `rivet.rktd` when composing the channel manifest.

For the developer preview on the `main` branch this is enough; production
steps continue below.

## 2. Update key embedding

The Ed25519 verification key ships as source, not a build-time
substitution: `app/update.rkt` commits `current-update-public-key-hex`
(DER SPKI, hex) and `update-key-id` permanently — the public half only
enables verification, so developer builds and release builds verify
updates identically and there is no key step in packaging. The private
half never enters a checkout: it lives in the keys vault and in the
`RIVET_UPDATE_PRIVATE_KEY` repository secret (PEM; the release pipeline
converts it to DER for `raco rivet release` and for the channel-manifest
signer, which takes the matching `RIVET_UPDATE_KEY_ID`).

Key rotation: generate a new pair (§0), replace the committed hex and key
id —

```bash
xxd -p update-public.der | tr -d '\n'   # → current-update-public-key-hex
```

— ship a client trusting the **next** public key first, then sign
releases exclusively with it. The `key_id` field is what makes the
overlap safe.

## 3. Tagged pipeline

```bash
git tag -s v0.1.0 -m "Fulcrum 0.1.0"
git push origin v0.1.0
```

`.github/workflows/release.yml` then, per platform matrix (macOS arm64 +
x64, Windows x64, Linux x64):

1. validates the tag against `VERSION`, `rivet.rktd` and the changelog;
2. validates independent signing modes; Linux artifact and update-feed keys
   are mandatory, while the Windows publisher certificate is conditional on
   its opt-in mode;
3. runs production signing and `raco rivet verify --production` whenever the
   platform's production credentials are present — on macOS that means the
   five family Apple secrets; a tag push without them fails closed. Windows
   unsigned builds use `raco rivet release --development` and must not claim
   Authenticode trust; macOS ad-hoc exists only for manual dispatch smoke
   builds and must not claim Developer ID notarization;
4. renames artifacts to the versioned, architecture-suffixed release names
   (`fulcrum-<version>-macos-<arch>.dmg` / `.zip`,
   `fulcrum-<version>-windows-x64.msi`,
   `fulcrum-<version>-linux-x64.tar.gz`), writes a `.sha256` sidecar per
   artifact, and retains those individual checksum sidecars in the CI artifacts;
5. the publish job signs one channel manifest pinning all artifacts
   (macOS clients are served the portable zip for their architecture),
   verifies the exact payload bytes, assembles one deterministic
   `SHA256SUMS` from the sidecars, and drafts the GitHub release with
   only versioned binaries, signed update metadata and that checksum file. Manifest URLs are tag-pinned
   (`releases/download/<tag>/<artifact>`) — never re-upload under an
   existing version URL. The app itself fetches `manifest.json` under its
   `update-base-url` setting (default
   `https://github.com/turlamai/fulcrum/releases/latest/download`),
   so only publishing a non-prerelease release changes the stable feed.

### Independent platform trust policies

The stable release must have `FULCRUM_STABLE_RELEASE_APPROVED=true` before
an immutable tag is created **and** before its draft is promoted.
Platform signing policies are independent:

| Platform | Credential-driven state | Opt-in mode |
| --- | --- | --- |
| macOS (arm64/x64) | Five family secrets present → Developer ID signed, notarized and stapled; absent → ad-hoc on manual dispatch builds only, **tag pushes fail closed** | No variable switch |
| Windows (x64) | `FULCRUM_WINDOWS_SIGNING_MODE=unsigned`; no paid certificate | `authenticode`, with PFX and timestamp service |
| Linux (x64/arm64) | Project-owned Ed25519 detached artifact signatures | No developer account required |

Unset Windows values default to `unsigned`; invalid values fail closed.
The legacy `FULCRUM_UNSIGNED_RELEASE_APPROVED` toggle is **not** a signing
policy: the tag preflight must not couple optional Windows signing to
macOS notarization. Tag releases always require the five family Apple
secrets plus the Linux artifact and Ed25519 signed-update-feed secrets,
and a matching update-feed key ID.
`scripts/version-release-preflight.sh` and its test matrix enforce these
requirements before tagging. Release notes must disclose Gatekeeper and
SmartScreen warnings when applicable; never instruct users to disable
system-wide protections.

### Release candidates

An RC tag uses `v<VERSION>-rc.N`, but `VERSION`, `rivet.rktd`, native package
metadata and artifact names keep the numeric `VERSION`. This is intentional:
WiX MSI and RPM package versions cannot safely use a SemVer prerelease suffix.
Create the version bump on a release-candidate branch, let that exact commit
pass CI and Product Acceptance, then tag that commit. Do not merge the numeric
version bump to `main`, because the main-branch version gate would create a
stable tag automatically.

The workflow marks an RC as a GitHub prerelease and explicitly sets
`make_latest=false`. Therefore the stable updater URL
`releases/latest/download/update-manifest.json` keeps resolving to the last
stable release. User-facing installers use canonical versioned asset names
selected from the stable release page, not duplicated versionless aliases. The RC still carries its own
tag-pinned signed manifest for inspection and manual installation.

Because the installed native package version is numeric, an RC consumes that
number. To test online update from the RC, the next candidate or stable release
must increment the numeric version (for example, candidate package `0.13.3`
updates to stable `0.13.4`); republishing `0.13.3` as stable would correctly be
treated as the same version and would not exercise an update.

**Always required** GitHub Actions secrets for stable and RC tags:
`LINUX_SIGN_PRIVATE_KEY`, `LINUX_SIGN_KEY_ID`,
`RIVET_UPDATE_PRIVATE_KEY`, `RIVET_UPDATE_KEY_ID=fulcrum-2026-10`, and the
five family Apple production secrets
`DEVELOPER_ID_P12_B64` (base64 of the Developer ID Application P12),
`DEVELOPER_ID_P12_PASSWORD`, `ASC_KEY_ID`, `ASC_ISSUER_ID` and
`ASC_KEY_P8_B64` (base64 App Store Connect API key). Tag releases are
production-signed or fail closed. Both architecture jobs import the p12
into temporary keychains, discover the Developer ID identity, and store the
`ASC_NOTARY` notarytool profile. The remaining publisher credential is
conditional on its opt-in mode:

- Windows `authenticode` mode only: `WINDOWS_CERTIFICATE_PFX`: base64 of an Authenticode code-signing PFX
  and `WINDOWS_CERTIFICATE_PASSWORD`. Set the repository **variable**
  `WINDOWS_TIMESTAMP_URL` to a working RFC 3161 service URL.
  Certificates requiring interactive hardware-backed signing need a
  separately provisioned trusted runner; do not export hardware keys.
- `LINUX_SIGN_PRIVATE_KEY`: PEM-encoded **artifact signing** Ed25519
  private key and `LINUX_SIGN_KEY_ID`. This is separate from the update
  feed signing key. Production Linux release ships the detached signature
  `fulcrum-<version>-linux-<arch>.tar.gz.sig`; DEB/RPM repository metadata
  signing is not implied by this signature.
- `RIVET_UPDATE_PRIVATE_KEY`: PEM Ed25519 feed signing private key, plus
  `RIVET_UPDATE_KEY_ID=fulcrum-2026-10`. The ID and its public key must
  match the trust root embedded in shipped clients.

Before merging any `VERSION` bump, set the repository variable
`FULCRUM_STABLE_RELEASE_APPROVED=true` **only when you intend to ship**.
`version-release.yml` checks this authorization and the presence of all
credentials **before creating the immutable tag**. Set it back to `false`
after publishing. Manually pushed stable tags are also blocked at the draft
promotion step unless this variable is `true`. Without approval, the
version-bump workflow fails and does not create a tag.

Private keys are materialized only in ephemeral runner directories and
never committed. Credentials **required by the selected platform mode**
fail closed, not credentials for unrelated optional modes; on macOS the
five family secrets are always required for tag releases. Ad-hoc/unsigned
is an explicit policy, never a fallback from a failed trusted build.
The manifest base URL comes from the tag, not a secret.

## 3a. macOS drag-to-install DMG (Taskly-parity)

Fulcrum now uses the **Rivet v0.12.0 branded Finder DMG** instead of the
default single-folder disk image. `rivet.rktd` declares
`dmg-background = assets/dmg-background.png`; its editable, reviewable
source is `assets/dmg-background.svg`. Finder positions the 80-point app
and Applications-link icons at (130,160) and (450,160) in a 660×400 window.

On each **macOS** tagged-release runner, after installing Rivet but before
`raco rivet release`, run:

```bash
brew install librsvg
bash scripts/render-dmg-background.sh
python3 scripts/check-macos-dmg-layout.py
```

The build fails if the artwork cannot be rendered or is not the expected
1320×800 (2×) PNG; the generated raster is intentionally gitignored.
Both `macos-arm64` and `macos-x64` run this check, and macOS CI
validates the SVG/raster contract as well.

**Release smoke test on a real macOS machine, both architectures:**
mount the actual packaged `.dmg`, check that the Finder window displays
Fulcrum.app and the Applications alias with the correct drag cue, drag the
app to Applications, eject the DMG, launch the installed copy, summon it
with the hotkey and check Accessibility permissions are requested only
when relevant. An app started directly inside a mounted DMG is **not**
the installed copy and must not be used as the updater acceptance path.

**Signing is a separate gate.** Only credential-configured
(production-signed) runs may claim notarization; require actual installed
macOS app and DMG evidence from `codesign --verify --deep --strict`,
`spctl --assess` and `xcrun stapler validate`. Ad-hoc builds (dispatch
smoke only) are not trusted publisher signing; disclose their limits
rather than disabling Gatekeeper, clearing quarantine or calling them
notarized.

---

## 4. Publish checklist

- [ ] Draft release notes link the changelog section.
- [ ] `manifest.json` verifies against the embedded **Ed25519 feed
      public key**; `raco rivet verify --production` is a separate
      platform-package/signature verification, not a manifest verifier.
- [ ] The macOS/Windows trust states match the Release warning:
      Developer ID/notarization and Authenticode checks only when enabled;
      ad-hoc and unsigned limitations clearly disclosed otherwise.
      Both Linux tarballs have verified Ed25519 detached signatures.
- [ ] Install the signed artifact on a clean VM for each OS; run one
      search, one clipboard copy, one plugin query, one update check.
- [ ] In-app update check reports the new version on the previous release.
- [ ] Site pricing/download links still match reality.

## 5. Rollback

If a release must be pulled: publish a new patch version and point
`stable/manifest.json` at it (signed, with `rollback` metadata referencing
the pulled version). Never re-upload an artifact under an existing version
URL — hashes are pinned by the signed manifest.
