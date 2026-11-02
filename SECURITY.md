# Security Policy

Fulcrum handles sensitive data by design (clipboard history, snippets,
on-device search). Reports are taken seriously and handled quickly.

## Reporting a vulnerability

Email **security@jrtx.site** (or open a GitHub security advisory on this
repository). Include reproduction steps, affected versions, and your
assessment of impact. You will get an acknowledgment within 72 hours.

Please do not open public issues for security problems.

## Scope

In scope:

- The RVT1 transport and embedded-runtime boundary (`app/backend.rkt`,
  Rivet's protocol layer)
- The FPP1 plugin sandbox claims in `docs/plugins.md` — v1 permissions are
  advisory and plugins are trusted code with user-level privileges; claims
  beyond that are bugs in the docs, not just the code
- Update trust: manifest verification, key handling (`app/update.rkt`,
  `rivet/distribution`)
- Local data handling: clipboard/snippet stores, settings, the
  persistent filename cache and native single-instance lease
- The hosts' handling of Events (`open-url` must stay http(s)-only;
  `copy-to-clipboard` must not leak to logs)

Out of scope: social engineering, physical access attacks, and
vulnerabilities in the OS WebView (Fulcrum does not use one).

## Design notes reviewers should know

- The backend binds no network ports; RVT1 uses inherited process handles.
- **Plugins are trusted, user-privileged code, not sandboxed apps.** FPP1
  creates a bounded process per call; FPP2 permits a persistent daemon.
  Permission declarations are advisory in both protocols and are **not**
  enforced by an OS sandbox. Install only plugins whose code and publisher
  you trust. A subprocess boundary contains crashes and supports timeouts,
  but cannot by itself prevent filesystem or network access.
- **Updates and installer trust are different layers.** Rivet verifies the
  Ed25519-signed update feed and downloaded installer bytes before initiating
  an in-app handoff. The macOS installed-app handoff, Windows MSI handoff and
  Linux AppImage replacement have distinct platform lifecycle requirements.
  Linux deb/rpm upgrades are owned by the package manager.
- The signed update feed **does not imply a publisher-signed installer**.
  macOS developer-preview artifacts are currently ad-hoc signed (not
  notarized); default Windows packages are not Authenticode-signed.
  Do not represent either as an independently trusted publisher identity.
  See [release signing policy](docs/release-signing-policy.md).
- Clipboard history and snippets are stored locally; AI keys are stored
  separately and excluded from sync. File names and paths from the local
  index are also not synced. Access controls ultimately depend on OS user
  permissions and filesystem protection; never assume all local storage
  is encrypted or protected from programs running as the same user.
- Windows/Linux/macOS activation and shutdown use Rivet's single-instance
  and host lifecycle mechanisms; they do not rely on a common PID file.
  Treat hung-owner recovery and installer-driven shutdown as security-
  relevant reliability paths.
## Supported versions

Security fixes land on `main` first and ship with the next tagged release.
During the 0.x developer preview there is no LTS branch.
