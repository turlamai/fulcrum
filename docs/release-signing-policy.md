# Fulcrum release trust policy

Fulcrum's platform executable signing is independent of its **Ed25519-signed
update channel**. This policy does not authorize publishing a release. Tag
builds still require successful CI and Product Acceptance on the tagged
commit, and stable promotion requires
`FULCRUM_STABLE_RELEASE_APPROVED=true`.

macOS production signing is credential-driven (family contract,
turlamai/taskly#122 template): when the five account-level secrets
(`DEVELOPER_ID_P12_B64`, `DEVELOPER_ID_P12_PASSWORD`, `ASC_KEY_ID`,
`ASC_ISSUER_ID`, `ASC_KEY_P8_B64`) are present, every macOS packaging run
Developer ID-signs, notarizes through the fixed `ASC_NOTARY` keychain
profile, and staples. Without them, manual dispatch builds stay ad-hoc and
**tag pushes fail closed** — a release never ships ad-hoc. There is no
macOS repository-variable mode switch.

| Repository variable | Default when unset | Opt-in signed value |
| --- | --- | --- |
| `FULCRUM_WINDOWS_SIGNING_MODE` | `unsigned` | `authenticode` |

The Windows variable is validated before packaging. Unknown values fail the
workflow. **Windows signing is not a prerequisite for Apple signing.**

- **macOS:** builds without credentials keep an ad-hoc code signature for
  integrity, **not** a trusted Developer ID signature or Apple notarization;
  Gatekeeper may warn. That state exists only for manual dispatch smoke
  builds, never for releases. With the five secrets present, the trusted
  release path (Developer ID identity, hardened runtime, notarization,
  stapling, Gatekeeper checks) applies automatically. Missing or invalid
  credentials on a tag push fail closed, never silently fall back to
  ad-hoc. Do not claim notarization until a real certificate-backed release
  job verifies it.
- **Windows:** unsigned MSI and portable ZIP are normal distribution
  formats. SmartScreen warnings must be disclosed. A paid Windows signing
  certificate is not required. Later, `authenticode` mode requires the PFX,
  password, timestamp URL and production verification.
- **Linux:** the existing portable/native formats require no paid
  developer account. Linux detached Ed25519 artifact signatures and
  checksums remain mandatory in the release workflow.
- **Updates:** a separately signed Ed25519 update manifest, pinned key
  identifier, payload hashes, and client-side signature/version/rollback
  checks remain mandatory regardless of platform executable-signing mode.
  Never expose a private signing key in PR jobs.

A draft release receives an explicit trust-status notice before promotion
whenever either desktop platform is in no-certificate mode. The existing
quality and manual stable-approval gates remain. This change does not
publish, tag or promote any release.
