# Fulcrum release trust policy

Fulcrum's platform executable signing is independent of its **Ed25519-signed
update channel**. This policy does not authorize publishing a release. Tag
builds still require successful CI and Product Acceptance on the tagged
commit, and stable promotion requires
`FULCRUM_STABLE_RELEASE_APPROVED=true`.

| Repository variable | Default when unset | Opt-in signed value |
| --- | --- | --- |
| `FULCRUM_MACOS_SIGNING_MODE` | `adhoc` | `developer-id` |
| `FULCRUM_WINDOWS_SIGNING_MODE` | `unsigned` | `authenticode` |

Both variables are validated before packaging. Unknown values fail the
workflow. **Windows signing is not a prerequisite for Apple signing.**

- **macOS:** the default app has an ad-hoc code signature for integrity,
  **not** a trusted Developer ID signature or Apple notarization. Gatekeeper
  may warn. After enrolling in the Apple Developer Program and provisioning
  secrets, explicitly select `developer-id`. The trusted release path
  requires a Developer ID identity, hardened runtime, notarization, stapling,
  and Gatekeeper checks. Missing or invalid credentials fail closed, never
  silently fall back to ad-hoc. Do not claim notarization until a real
  certificate-backed release job verifies it.
- **Windows:** unsigned MSI and portable ZIP are normal distribution
  formats. SmartScreen warnings must be disclosed. A paid Windows signing
  certificate is not required. Later, `authenticode` mode requires the PFX,
  password, timestamp URL and production verification. It can be enabled
  without changing the macOS signing mode.
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
