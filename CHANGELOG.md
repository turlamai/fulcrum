# Changelog

All notable changes to Fulcrum are documented here. Versions follow
[SemVer](https://semver.org/); the `build` number in `rivet.rktd` increments
independently per platform packaging run.

## [Unreleased]

### Changed

- The updater now runs on `rivet/distribution`'s managed `make-updater` state
  machine: candidate selection, the download worker, the stall/total
  watchdog, cancellation and the terminal state are shared family code.
  Fulcrum supplies its feed policy and staging path; the host handoff ticket
  reads the authenticated candidate through `updater-candidate` (artifact
  SHA-256, installer kind, rollback policy). The `update-state` wire shape is
  unchanged.

## [0.19.0] - 2026-11-13

### Added

- **Design baseline v2 across all three hosts** (prototype `0ca2178`).
  Text and glyphs move to the readable amber pair — `#9A4B06` light /
  `#F0A64B` dark, both ≥ 4.5:1 — with raw `#D97706` reserved for fills;
  hover becomes a neutral tint that never competes with the amber
  selection; typography converges (row title 13 medium, subtitle 11.5,
  AI/detail body 12.5); the settings window is 720 × 520 everywhere.
- **Hit highlighting.** Query matches are marked inside result rows —
  the readable amber at weight 650, no background fill — from per-row
  match ranges the engine ships on the wire. Rows that matched only via
  keywords render unhighlighted rather than wrong.
- **Masked clipboard credentials.** Clipboard content that looks like a
  credential (provider tokens, PEM key headers, long secret
  assignments) travels masked with a localized `Sensitive` badge;
  Ctrl/⌘+Y reveals the raw text through a local-only RPC — never
  logged, never synced, and dropped when the result list refreshes.
- **IME composition guard** on every host: while the input method holds
  preedit, Enter / arrows / Tab / Esc / ⌘K belong to the IME — no
  execution, no selection move — and the first Esc cancels only the
  composition.
- **Focus contract.** The results list is one tab stop as a whole: Tab
  completes when a row offers it and moves focus otherwise, Shift+Tab
  returns to the query, focus draws a 2 px accent-text ring, and Esc
  from the actions layer restores the originating row by item id.
- **Actions mode bar.** ⌘K shows an esc pill, the row being acted on
  and the layer badge; the selected action swaps the amber fill for a
  1.5 px inset ring so the layer is visible, not read.
- **Dynamic panel height.** The panel grows with its results — 240
  empty, capped at 12 visible rows / 640 points — and the Maximum
  results setting tops out at 12 to agree with the visual cap.
- **Design self-check page** (`design/prototype/audit.html`): the state
  matrix — hit highlight, IME composition, focus order, offline /
  rate-limit / AI running·done·401·429, sensitive clipboard, over-limit
  CJK and the row cap — that every new state enters before hosts
  implement it.

## [0.18.1] - 2026-11-11

### Fixed

- **Windows: the tray's "Check for updates" now actually opens the
  settings window.** The menu handler called `Activate()` on the
  settings window while the tray menu itself held the foreground right,
  so Windows demoted the activation to a taskbar flash — the launcher
  hid and no window appeared (the About window needed the same fix
  earlier). `ShowSettings` now claims the foreground explicitly for the
  settings HWND, matching macOS, where the tray item opens a visible
  update panel.
- **All platforms: the update surface speaks the UI language.** The
  launcher's update rows (check/download/progress/install/manual-hint),
  their status notifications, the Windows settings window's navigation
  and Sync & Updates card, and the macOS settings' Updates section now
  read from the shared bilingual string table — a Chinese system shows
  Chinese, matching the "language follows the system" promise the
  settings row makes.
- **Windows: update housekeeping no longer lets one failure swallow the
  other.** Startup's stale-attempt sweep and failure-marker consumption
  each run in their own try: a throw in the sweep (a hostile
  environment leaving the update root unreadable) previously skipped
  marker consumption, re-reporting the same stale failure on every
  launch.

## [0.18.0] - 2026-11-11

### Added

- **Windows: live file watching, run by the host process (issue #211
  spike).** The v0.17.0 investigation showed the armed watchers
  reconcile under plain racket.exe CI but never in the packaged build —
  the embedded runtime's scheduler is the delta. This release sidesteps
  that layer: each catalog root (user profile + fixed local volumes) is
  watched in the host process with overlapped `ReadDirectoryChangesW`,
  changed directories are batched and forwarded over the new
  `fs-reconcile` RPC to the backend — the backend stays the single
  owner of the catalog database, the host only names directories. A
  successful reconcile bumps the nudge, so open queries repaint through
  the existing path. Files created while Fulcrum runs now appear in
  `find` results within seconds, on Windows, without a restart.
- **Windows settings: a working update card in Sync & Updates.** Check
  now, download with percent progress, quit-and-install — driven by the
  update RPCs, with the tray's "Check for updates" entry opening this
  window on the Sync & Updates tab and running the check (replacing the
  old hidden-launcher status line, matching macOS behavior).
- **macOS settings: a "Check for Updates Now" button** in the Updates
  section, opening the native update panel.

### Fixed

- **All platforms: Enter re-runs the FINAL query.** Fast typing outran
  the async result repaint, so Enter acted on the selected row of a
  previous generation: `!g 豆包` searched `!g`, `!g vflash 刷写`
  searched only `vflash` (user-reported). All three hosts now re-run
  the final query on Enter and act on the freshest row.
- **All platforms: summoning the launcher keeps the previous query**
  instead of wiping it back to an empty box, so a mis-dismissed search
  survives the next wake (user-reported).

## [0.17.0] - 2026-11-10

### Added

- **Windows: `find` answers from the live catalog without launching a
  platform search process per query.** Once the catalog's scan
  completes, the persistent index is the authoritative source —
  queries that the catalog already answers return in milliseconds
  instead of spawning `powershell.exe` (the per-query spawn budgeted
  up to nine seconds). Thin or still-scanning catalogs keep the
  platform stage to fill gaps and seed the index; macOS and Linux
  behavior is unchanged, where Spotlight and plocate are already
  instant.

### Deferred

- **Windows: live file watching stays out of this release.** The
  watcher machinery (issue #99, #190) is restored in the tree with its
  full test suite — overlapped-I/O `ReadDirectoryChangesW` per scanned
  root, engine wiring, the delete-pending chain — and arming is
  deferred: on the packaged embedded runtime the armed watchers
  reconcile correctly under plain racket.exe CI but do not reconcile
  in the shipped build (verified on a clean Windows VM: catalog ready,
  new files in watched roots never land). Scanning behavior remains
  the proven once-per-process model; restart Fulcrum to pick up
  filesystem changes. The investigation continues under #99 and #211.

### Fixed

- **All platforms: explicit `find` queries ranked file hits below
  unrelated fuzzy matches.** App rows score 100+ before relevance is
  considered, so a `find probe` painted weak app matches first and the
  actual file hits sat below the fold — indistinguishable from no
  results at all (issue #193 follow-up; the #116 acceptance criterion
  is that the probe hit is the first row). A `find <text>` query names
  the file surface outright, so its rows now lead and every other
  provider follows in score order.
- **Windows: typed `find` queries never showed file results.** Every
  keystroke starts a search, and the searches raced for one global
  cancel slot that carried no query identity — an older request
  claiming late cancelled the *newest* query's platform search. The
  cancelled response also discarded the local-catalog hits it had
  already collected (issue #193). Claims now supersede only another
  claim for the same query text; a cancelled platform stage degrades
  to the catalog hits exactly like a timeout does.
- **Windows: the launcher jumped every ~750 ms while the catalog
  crawled.** The host repainted per revision push and rebuilt the
  whole results list, snapping the selection back to the top (issue
  #194). Revision pushes now coalesce into one query re-run, identical
  rows no longer rebuild the list, and real rebuilds keep the
  selection.

## [0.16.1] - 2026-11-09

### Fixed

- **Windows: typed `find` queries never showed file results.** Every
  keystroke starts a search, and the searches raced for one global
  cancel slot that carried no query identity — an older request claiming
  late (RVT1 workers run concurrently, so claim order is not request
  order) cancelled the *newest* query's platform search, which on
  Windows takes seconds. The cancelled response also discarded the
  local-catalog hits it had already collected, so the launcher stayed on
  unrelated app rows with no error — during the initial catalog crawl
  and after it (issue #193). Claims now supersede only another claim for
  the same query text; a cancelled platform stage degrades to the
  catalog hits exactly like a timeout does; a cancelled search with an
  empty cache paints the honest coverage/progress row instead of
  nothing.
- **All platforms: explicit `find` queries ranked file hits below
  unrelated fuzzy matches.** App rows score 100+ before relevance is
  considered, so a `find probe` painted seven weak app matches first and
  the actual file hits sat below the fold — indistinguishable from no
  results at all (issue #193 follow-up; the #116 acceptance criterion is
  that the probe hit is the first row). A `find <text>` query names the
  file surface outright, so its rows now lead and every other provider
  follows in score order — the same surface principle the backend
  already applies to "settings" and "update".
- **Windows: the launcher jumped every ~750 ms while the catalog
  crawled.** The initial build nudges `search-revision` about once a
  second and the host repainted per push — clearing and rebuilding the
  whole results list and snapping the selection back to the top for the
  entire crawl (issue #194). Revision pushes now coalesce into one
  query re-run 400 ms after the last push; a re-run that returned the
  rows already on screen no longer rebuilds the list; a real rebuild
  keeps the selection when the selected row survives.

## [0.16.0] - 2026-11-06

### Changed

- **Packaging dependency bump.** `RIVET_PIN` moved to c29e62f for the
  rpm 6 `%files` quoting fixes (rivet#431): rpm 6 treats quoted entries
  as literals instead of globbing them, which broke the x86_64 rpm leg
  on newer builders.

### Not shipped

- The live-filesystem watcher PRs (#189, #191, #192) were merged and
  then reverted before tagging (commit 6e679da), so v0.16.0 ships the
  same product behavior as v0.15.2 plus the packaging bump above.
  Windows live watching remains upcoming under #99; the revert pulled
  its freeze/crash fixes (#190) back out with it. (The published
  release notes carried this correction; this section now matches it.)

## [0.15.2] - 2026-11-05

### Changed

- **Windows data volumes are now searchable.** `find` previously reached
  only the user folders (plus whatever the OS search index happened to
  cover), so files on data partitions or outside known folders —
  `C:\FulcrumTest\report.txt`, anything on D:/E: — were unfindable while
  macOS Spotlight and Linux plocate cover whole disks. Every fixed local
  volume except the system drive is now indexed full-depth into the same
  local catalog (batched, incremental, refreshed live as first-use scans
  progress). Mapped network shares and removable media are excluded from
  walks by drive type; a PowerShell failure degrades to home-folder
  coverage honestly, and volume metadata directories are never indexed.

### Fixed

- **The UI language follows the operating system by default.** Every
  fresh install previously rendered Chinese regardless of the user's
  locale, leaving English users to discover a language switch on their
  own. A new `system` value is now the default and each host resolves it
  against the OS locale (preferred languages on macOS, the UI language
  on Windows, `LC_ALL`/`LANG` on Linux); `zh` and `en` still force a
  language, and Settings offers 跟随系统 as the first choice.
- **The Windows tray "Check for Updates…" gives visible feedback.** The
  result was written into the hidden launcher window's status line, so
  triggering the check from the tray showed nothing at all — no
  reaction, no result, unless the panel happened to be opened later.
  The tray entry now summons the launcher first, so the check and its
  result land on a visible status line.

## [0.15.1] - 2026-11-05

### Fixed

- **Chinese queries search again in packaged apps.** Since 0.14.0 every
  installed app failed the first query containing Han characters with
  `open-input-file: cannot open input file`: the pinyin recall table
  shipped as a runtime-path data file whose `data/` subpath the desktop
  packager flattened, so packaged apps could not open it (English search
  never touched the table, which is why CI acceptance stayed green).
  The table now ships as a compiled module constant embedded in the
  release binary — it cannot be lost in packaging — with the generator
  updated to emit that module and the table integrity pinned by a
  regression test.
- **Update check results are visible again on all three platforms.**
  The backend delivers run-action feedback (update check results,
  settings toggles, plugin installs) as a `notify` event, but every
  host's event dispatcher dropped it: the check ran, verified the signed
  manifest and persisted `update-last-check`, while the status line kept
  its "Running…" placeholder forever. Found during the v0.15.1 VM
  validation pass on the Linux deb; macOS drops it on the launcher-row
  path and Windows on the same dispatcher. Each host now writes the
  notify payload into its status line, pinned by a three-source static
  contract test.

## [0.15.0] - 2026-11-04

### Added

- **Optional native Start at login on macOS, Windows, and Linux.**
  The login item is off by default, managed by each OS for the current
  user and never synced. Repeated login activation does not summon the
  existing launcher. Settings surfaces show actual registration state
  and native errors; Linux uses an XDG desktop entry, Windows HKCU Run,
  and macOS ServiceManagement Login Items. Migrated safely from the
  obsolete v0.14.1 release PR #104.

### Changed

- **Rivet pin moved to 1a38a5c**, which standardizes plain DMG
  drag-to-Applications installation, hardens macOS DMG detachment and
  qualifies Windows MSI repair, and hardens packaging hashing behind a
  lazy Ed25519 provider. This also carries the earlier MSI-repair pin
  (9355362) adopted across the build and acceptance jobs in #165.

### Fixed

- **macOS update dialog Close now actually dismisses the window.** The
  previous Close buttons only changed a SwiftUI boolean while the real
  AppKit window stayed open. Since the idle state reused the checking
  spinner, closing a finished update check looked like an endless new
  check. The Close buttons and native title-bar close now hide the same
  window, idle no longer spins, in-flight dismissed checks cannot revive
  the panel, and reopening does not launch duplicate checks. A completed
  manual check also suppresses the delayed startup auto-check, avoiding
  immediately repeated network requests. Closing during download keeps
  the download running; Cancel remains explicit.
- **Stable releases promote unattended again.** For a draft referencing an
  already-minted tag, GitHub detaches the tag name server-side and reports
  an `untagged-<hash>` placeholder until publication — so the release
  workflow's draft-identity gate demanded a field GitHub itself replaces,
  and every gate-minted stable promotion failed at the last step
  (v0.14.2's publish job; manual recovery applied). Draft identity is now
  anchored on the numeric REST id and accepts both the requested and
  detached tag-name shapes; version, commit, channel and draft state stay
  strict, and the post-publication check still demands the exact tag name
  on a published release. Stable alias uploads address the draft by its
  REST id with delete-then-upload clobber semantics instead of resolving
  through the detached tag name, and the offline contract tests and the
  CI static gate pin the new shape in both directions.
- **Release assets ship from a strict canonical inventory.** The release
  pipeline stages verified canonical payloads without duplicate aliases,
  requires the complete native Linux installers (deb/rpm/AppImage) and
  signed consolidated checksums, and fails closed before publication
  when staged assets are missing, duplicated, mismatched or extra. The
  site's download resolver selects canonical versioned assets with a
  safe fallback and recognizes SemVer tags.
- **File search reports what happened instead of pretending to be
  empty.** An empty `find` result now states that the local index is
  still building (with progress) or which scopes were searched, and an
  inaccessible local index is reported as a failure rather than folded
  into a valid zero-hit answer.
- **Live `find` queries refresh while the first-use catalog builds.**
  Catalog batches feed the shared search-revision nudge, so results
  arrive in the open query without retyping.
- **Windows file search keeps UNC shares and Unicode names in the local
  index.** Indexed UNC paths lost their server segment and non-ASCII
  filenames were mangled on the way out; both survive transport now,
  covered by regression tests.
- **Windows explicit `find` restores known-folder-first coverage.** The
  Windows Search query could burn its whole time budget before reaching
  Desktop/Documents/Downloads, yielding nothing on normal machines;
  known folders are checked first again without regressing UNC parsing.
- **Search results stay monotonic under concurrent workers.** Plugin and
  file-index workers published revision nudges out of order, which could
  leave a stale result set on screen; revision publication is serialized
  and failure recovery is covered by concurrency tests.
- **Windows native RPC workers keep the backend and API alive.** Detached
  Settings and launcher RPC workers could outlive the objects they
  touched and capture dangling pointers; ownership is now held across
  the detached lifetime.
- **Windows tray About opens as a real window again.** The tray menu's
  About entry rendered into a window that never got activated or shown,
  so nothing appeared. About is now its own independently visible
  top-level window, asserted by packaged acceptance without summoning
  the launcher.
- **Windows survives a stuck resident instance and MSI replacement.** A
  pending launch recovers from a stale resident lease within seconds and
  reports a nonresponsive owner instead of hanging, and the resident
  process now exits cooperatively when MSI Restart Manager needs to
  replace the app during install or upgrade.
- **Launching Fulcrum from the desktop shows the launcher on Linux.** A
  desktop relaunch of the resident instance only toggled visibility when
  explicitly requested (`--toggle`), leaving ordinary launches silent on
  some sessions. Desktop launches now always present the panel; toggling
  stays an explicit act.
- **GNOME Wayland portal hotkey failures stay visible on Linux.** The
  portal's app-id rejection used to be overwritten once the backend
  reported Ready, hiding why the summon hotkey was dead; the failure is
  preserved through readiness and settings refresh, with the manual
  shortcut fallback documented.
- **Every shipped link points at the canonical `turlamai` owner.** About
  dialogs (Windows issue reporter, Linux and macOS hosts), docs,
  registry links and the update-feed default now name the renamed owner;
  the old name is again registrable, so stale links would break silently.
- **Stable tags are gated by an independent signing preflight.** Before
  minting an immutable tag, each platform's signing mode (macOS ad-hoc
  or Developer ID, Windows unsigned or Authenticode) is validated with
  its own required credentials, decoupling optional paid signing from
  the release decision.

## [0.14.2] - 2026-11-02

### Fixed

- **Windows Settings title bar shows the app icon again.** The window
  passed its exe path to `AppWindow.SetIcon`, whose String overload wants
  an `.ico` file — the call failed silently inside the constructor's
  `try/catch` and the slot stayed blank in v0.14.1. It now uses rivet's
  `ApplyAppIcon` helper (already in the pinned rivet), which loads the
  embedded `RT_GROUP_ICON` resource and hands `SetIcon` an `IconId`;
  missing resources keep the native default. A static contract test
  rejects the exe-path overload and locks the helper in.
- **File search resolves UNC shares and POSIX paths from file URLs.** The
  URL-to-path decoder stripped every leading slash, so Windows Search
  results on UNC shares (`file://host/share/...`) lost their server and
  an absolute POSIX file URL collapsed into a relative path. Only the
  scheme and the drive's URI prefix are removed now; UNC, POSIX and
  arbitrary-volume forms survive intact, covered by regression tests.
- **The default update feed points at the canonical repository.** The
  backend's fallback feed URL still named the pre-rename
  `turinglambdaai` org path, which serves no releases — installs relying
  on the default would never see an update. It now uses `turlamai`,
  matching the shipped updater configuration.
- **Draft releases publish reliably.** The release workflow pins the
  created draft's numeric REST id and commit SHA and verifies that exact
  identity before uploading stable aliases and before promotion, and the
  gate refuses untagged drafts early instead of failing verification with
  a confusing mismatch after promotion.

## [0.14.1] - 2026-10-30

### Changed

- **Rivet pin moved to 79a01a4**, which makes DMG packaging resilient to
  slow volume teardown on CI runners: the installer now probes the mount
  point, retries the graceful detach across the busy window, treats an
  already-torn-down volume as success, and only then falls back to the
  forced detach. The pin also renames the server-absent emit error to
  "no Rivet event emitter…"; the test asserting the old wording follows
  it.

### Fixed

- **Recents no longer remember what you typed.** Only stable launcher
  identities — app launches, system commands and window commands — are
  recorded in the recent list; web searches, quicklinks, snippets, and
  plugin arguments are excluded entirely, and startup scrubs legacy rows
  that stored them. Read-only environments keep the sanitized in-memory
  view when the cleanup cannot persist. Regression tests cover the
  omission and the cleanup.
- **Shortcut edits stay truthful when saves fail.** Setting or clearing a
  hotkey now publishes to the resident host only after the atomic write
  commits, so a failed save can no longer leave a binding live in memory
  that storage never accepted. The failed set and clear paths are covered
  by regression tests.
- **Windows launches and quits as one resident instance.** The first frame
  stays hidden until the resident process answers, a second launch targets
  the resident window by its native HWND and exits, and quitting shuts the
  resident process down so relaunches start clean. The packaged Windows E2E
  suite asserts the blank-frame, secondary-exit, quit and relaunch behavior.
- **Draft releases publish reliably.** The release workflow resolves GitHub
  draft releases by their REST database id — `gh release view` returns a
  GraphQL node id, which the publish endpoint rejects — instead of trusting
  the tag-name lookup that 0.14.0 relied on.

## [0.14.0] - 2026-10-27

### Added

- **Fast, durable file search on every desktop.** A SQLite-backed catalog now
  discovers files safely in the background, survives restarts, refreshes
  incrementally, and serves ranked results without launching a new operating
  system search process for every keystroke. Stale queries are cancelled so
  current input always wins.
- **Chinese pinyin recall.** Application names and file basenames match both
  full pinyin and initials while preserving exact and literal matches ahead of
  transliterations. The bundled transliteration data and generator are
  documented and licensed for reproducible builds.
- **Measured search performance.** Repeatable 100k-entry file-index and pinyin
  benchmarks accompany the architecture and regression suites.

### Changed

- **Rivet pin moved to a92877c**, which fixes the release pipeline crash
  that kept native Linux installers (deb/rpm/AppImage) out of 0.13.3 —
  they return in this release, alongside the AppImage update
  payload in the signed feed.
- **Native search remains platform-shaped.** Windows keeps WinUI 3, macOS keeps
  SwiftUI/AppKit, and Linux keeps GTK4 while sharing the same indexing, ranking,
  cancellation, pinyin, and history semantics in the Racket backend.

### Fixed

- **Reliable file-search process cleanup.** Concurrent stdout and stderr are
  drained without deadlock, process ports have one owner, and cancellation no
  longer reads an already-closed subprocess stdout port. The repeated
  `read-bytes-avail!: input port is closed` failure is covered by stress tests.
- **Windows tray menus open at the pointer.** Right-clicking the lower-left tray
  icon no longer anchors the context menu to the upper-right corner.
- **Clipboard history is reachable again.** Typing `clip` routes directly to
  clipboard history, and the macOS packaged-product test seeds the real system
  pasteboard before verifying that the copied value appears and can be chosen.
- **Usable, private native settings.** macOS now keeps every settings page
  visible in a fixed sidebar and records the summon shortcut instead of
  exposing the backend grammar. All three settings hosts hide internal
  updater transport fields, and rejected values no longer leak Racket
  contract errors into the UI.
- **Correct macOS installer artwork.** The DMG backdrop is authored at Finder's
  expected scale and verified against the real drag-to-Applications layout.
- **Reliable release publication.** Draft lookup uses GitHub's release API,
  stable Linux archive aliases follow the assets actually shipped, and every
  build runner provisions the SQLite runtime dependency required by the index.

### Verification

- The exact feature commit passed backend and native-host CI on Windows, macOS,
  and Linux; packaged WinUI/SwiftUI/GTK4 product acceptance (including macOS
  arm64 and x64); Windows MSI upgrade; Linux AppImage upgrade on x64 and arm64;
  and macOS signed-feed installation on arm64 and x64. Its merged main-branch
  commit repeated CI and Product Acceptance successfully. The release tag is
  created only after the exact 0.14.0 version commit repeats both gates.

### Distribution status of this release

- **This version uses the explicitly approved unsigned desktop channel** until
  Apple and Windows publisher accounts are provisioned: macOS packages are
  ad-hoc signed (right-click the app and choose Open on first launch), while
  Windows packages have no Authenticode signature and may trigger SmartScreen.
- **Integrity remains authenticated:** the update channel is Ed25519-signed,
  and both Linux tarballs carry detached Ed25519 artifact signatures. The
  fail-closed production gate remains the default after this release.

## [0.13.3] - 2026-10-22

### Added

- **Portable snippets backup and restore.** Discoverable `export snippets <file.json>` and `import snippets <file.json>` commands write a versioned JSON bundle and validate its contents before merging. IDs and timestamps survive round trips; exports refuse to overwrite existing files.
- **Native installed-update paths.** macOS supports authenticated update-to-installed-app handoff with rollback; Windows has verified MSI upgrade and failure/retry coverage; Linux x64/arm64 AppImages support atomic replacement, relaunch health checks, and rollback. DEB/RPM installations remain package-manager-managed and portable archives require manual handling.
- **macOS drag-to-Applications DMG.** Installer artwork and package-layout checks cover Apple Silicon and Intel release shapes.

### Security and release engineering

- **Production release gates.** Stable builds require macOS Developer ID signing and notarization, Windows Authenticode/timestamping, Linux artifact signatures and an independently Ed25519-signed update feed. Missing publisher credentials or release approval fail closed; development signing cannot silently become a stable release.
- **Candidate isolation.** RC tags publish only as prereleases and never replace stable download aliases or the stable update channel.
- **Verified update regression.** CI covers the real signed Windows manifest and MSI download/ticket path, native Windows installer lifecycle, macOS signed-feed installation, and Linux AppImage success/rollback on both architectures. Exact-tagged-commit CI and Product Acceptance are required before release packaging.

### Release verification

- Publisher-owned production signing credentials, real signed/notarized tagged artifacts, clean-install checks and final Windows UI-to-native MSI handoff acceptance remain mandatory before promoting this version to stable. A green developer CI run alone is not release approval.

### Linux installers in this release

- deb/rpm/AppImage are **absent from this release**: the pinned Rivet revision crashes when packaging a non-empty `linux-formats` list, so this release ships the tar.gz and portable zip for Linux (the v0.13.2 shape). The native installers return in the next release once the upstream fix is pinned.

### Distribution status of this release

- **This version ships as an explicitly approved unsigned release** (`FULCRUM_UNSIGNED_RELEASE_APPROVED=true`): publisher accounts are not provisioned yet, so macOS packages are ad-hoc signed (right-click the app → Open on first launch) and Windows packages carry no Authenticode signature (SmartScreen warns). The release notes carry the same warning.
- **What is still signed:** the Ed25519 update channel (every in-app update verifies byte integrity exactly as a signed release would), and both Linux tarballs keep their detached Ed25519 artifact signatures.
- The fail-closed production gate stays the default: without the explicit unsigned-approval variable, a stable tag with missing credentials still refuses to publish. The next release after the Apple/Windows publisher accounts are provisioned returns to mandatory Developer ID notarization and Authenticode signing.

## [0.13.2] - 2026-10-20

### Fixed

- **Reliable Ubuntu summon on X11 and Wayland.** Prefer native X11
  grabs, forward user-event timestamps to the compositor, protect newly
  presented launchers until first activation, and show actionable portal
  and GNOME shortcut-conflict recovery. Cover activation and keyboard
  focus in packaged acceptance.
- **Honest Linux file search.** Distinguish zero matches from missing search
  tools and scan timeouts, and recommend an indexed `plocate` installation
  when the unindexed fallback is too slow.
- **Three-platform interaction parity.** Converge focus-loss, click-to-select
  and double-click-to-run, Escape/actions grammar, native resident-menu
  entries and visible language changes across macOS, Windows and Linux.
  Make macOS CI build the actual native host and fail generated-client drift.
- **Consistent native launcher styling.** Align 680×460 panel geometry,
  native search typography, result density, amber row accents and selection
  affordances; add GNOME-owned Alt+Space conflict/retry regression coverage.
- **Localized settings labels.** Reuse the shared bilingual settings
  translations in the three native settings windows, including live changes
  of the active language. Native OS-specific help remains platform-idiomatic.

### Verification

All platform CI and packaged-product acceptance jobs must pass before
the version tag is created. The release's signed channel manifest and
checksums are assembled by the existing tag-driven workflow.


## [0.13.1] - 2026-10-20

### Fixed

- **The Linux panel stops fighting the window manager.** Dragging the panel
  no longer hides it mid-move — dismissal is deferred during pointer-held
  moves and brief focus handoffs, while sustained external focus loss still
  dismisses as before. Keyboard navigation scrolls the selected row fully
  into view (current-setting badges and an explicit empty state included),
  and theme/language edits apply immediately: light/dark GTK themes apply
  at once, System restores the desktop override, translations refresh, and
  a stale host-settings snapshot is rejected instead of overwriting newer
  edits — in the quick actions and the independent settings window alike.
  A `Restart Fulcrum` action joins the search box and the tray: it stops
  the backend, releases the single-instance lease, and executes the
  application again.

- **Windows updates finish themselves.** After a verified download the
  install row hands the MSI to `msiexec` (`/passive /norestart`) and
  Fulcrum relaunches when the installer is done, via the same
  quit-and-install handoff macOS already uses — previously Windows stopped
  at "open the updates folder and run it yourself". The manual row remains
  as a fallback, and a persisted installer failure is surfaced in the
  status UI on the next launch.

- **AppImage installs receive AppImage updates.** The signed channel
  manifest now publishes the x64 and arm64 AppImage payloads beside the
  tar.gz entries, and a Fulcrum actually running as an AppImage selects
  the authenticated AppImage entry for self-update; portable tar.gz
  installs keep their existing unfiltered path, and deb/rpm stay owned by
  the package manager.

- **The file-search gap row names the real missing tool.** Every platform
  has a search provider; the unavailability row — shown only when the
  platform's tool is absent from PATH — now says what to install per
  platform (mdfind / locate or find / PowerShell) instead of always
  claiming macOS Spotlight.

## [0.13.0] - 2026-10-17

### Added

- **A settings window on all three platforms.** Until now every setting
  lived behind query syntax (`settings`, `ai key …`, `ai model …`) — fine
  once you know it, invisible until you do. The launcher's leading
  `Settings…` row, the menu bar / tray menu, and ⌘, on macOS now open a
  native window over the same settings store: General (language, theme,
  hotkey, result count), Features (clipboard history and its limit, web
  search rows and default bang, snippet expansion, plugins and their
  timeout), AI (provider, model, base URL, persona, and the API key with a
  live readiness line — the key is saved locally and never crosses the
  wire back), Sync & Updates (sync folder, update feed). Edits apply as
  you make them; a value the backend refuses (a bad hotkey, an
  out-of-range number) is reported inline and the form falls back to the
  stored value. SwiftUI grouped form on macOS, a Mica window in the
  Windows Settings idiom, GTK on Linux. The query rows stay for typing.

- **Fifteen new first-party plugins — Raycast's developer-and-info belt.**
  The gallery grows from eleven to twenty-six. Offline tools: Hash
  (`hash`), Base64 (`b64`, encodes and detects-decodes), UUID (`uuid`),
  Password Generator (`pw`, entropy-source passwords and passphrases),
  Lorem Ipsum (`lorem`), Color (`color`, HEX/RGB/HSL with a readability
  hint), Case Converter (`case`, every casing at once), Number Base
  (`base`, dec/hex/oct/bin plus the Unicode glyph), Word Count (`wc`,
  clipboard-aware). Keyless web plugins: IP Address (`ip`), Hacker News
  (`hn`, Algolia), Dictionary (`define`, dictionaryapi.dev), npm Search
  (`npm`), Weather (`wx`, wttr.in), Wikipedia (`wiki`). All standard
  library only, all cross-platform, all smoke-tested through the wire
  protocol; the slowest feed stays inside the default plugin timeout.

### Fixed

- **The borderless panel could not be dragged.** Removing the titlebar
  (the caption X fix) also removed the only drag region, so the panel
  was nailed where it first appeared. The hit-test now answers
  `HTCAPTION` for the padding strip above the query box — the same deal
  as the macOS panel's transparent titlebar region — so the panel is
  movable by its top edge while rows and the query box keep their
  clicks.

- **The summoned panel did not accept typing until clicked.** Focusing
  the query box synchronously with `Activate()` races the activation —
  the focus lands on a window that is not foreground yet and is dropped
  when activation completes, so the caret never appeared and the first
  keystrokes went nowhere. The focus is queued for the tick after
  activation settles, and the hotkey path now claims the foreground it
  is entitled to (`WM_HOTKEY` grants it; the tray-open path and the
  first re-summon after a hide needed it too).

- **The Settings window's titlebar icon was blank.** WinUI shows nothing
  left of the title until an icon is set, and no window ever set one.
  The Settings window now points `AppWindow.SetIcon` at its own exe —
  the app icon lives there at resource ID 1 — so the staged tree and
  the installed build resolve it identically.

- **The launcher panel carried Windows caption buttons, and the X ended
  the whole app.** The macOS launcher panel ships with no close
  affordance (`.titled` without `.closable` — a floating card, Esc and
  defocus dismiss it) and the Linux host already routes its WM close
  button to hide instead of quit; Windows alone floated the system
  min/max/close trio over the acrylic, with no `Closing` handler — so
  the most prominent button on the panel terminated the process: tray,
  hotkey, updates and all. The border and titlebar are gone
  (`SetBorderAndTitleBar(false, false)`), dismissal is Esc / defocus /
  the tray, Alt+F4 hides instead of quitting, and the only exit remains
  the tray menu's Quit Fulcrum.

- **The Windows exe shipped with no icon and no file identity.** Rivet's
  build generates an icon + VERSIONINFO resource script and points
  `RIVET_WINDOWS_ICON_RC` at it, but the host project never compiled it —
  the ResourceCompile group exists in Rivet's scaffold template only, and
  this repo owns its own `RivetHost.vcxproj`. Every tray icon, taskbar
  button and Explorer listing fell back to the generic application icon.
  The group is in, and the exe now carries the Fulcrum icon (resource ID
  1, where `LoadIconW(MAKEINTRESOURCEW(1))` looks) and its version block.

- **The updater compiled only against the pinned Rivet.** `app/update.rkt`
  carried its own `platform-symbol`/`architecture-symbol` copies; when
  Rivet #258 centralized those helpers into `rivet/distribution`, any
  module requiring both — the updater test — failed to compile, invisible
  to CI (which installs the v0.11.0 pin) while every machine following
  latest Rivet broke. The local copies are gone and the shared helpers are
  re-exported, and the pin moved to a post-#258 commit. The same bump
  adopts Rivet's multi-installable artifact shape (`component` field,
  rivet 79b1bd8) — Fulcrum's feed carries one installable per platform,
  so the discriminator travels as `#f` — and commits a
  `rivet-schema.json` baseline with a CI schema-compatibility check, the
  `raco rivet doctor` fix-next item.

- **Plugins were dead after every restart.** The backend built its plugin
  manager before publishing the settings store, and the manager's first
  load consults the disabled-plugins setting — that consult raised, the
  load aborted, and every installed plugin stayed unloaded until some
  later reload (an install, a toggle). Boot order is fixed, and a
  regression test boots the production wiring against a fixture directory.

- **A disabled plugin could never be re-enabled.** The loader skipped
  disabled directories entirely, so the plugin center — which lists only
  loaded plugins — lost them the moment you turned one off. Disabled
  plugins are now parsed and listed with an Enable row.

- **Three settings that never worked.** The settings-list contract
  spelled booleans `#t`/`#f` while every host compared against `off`, so
  "off" never matched — snippet auto-expansion could not be turned off on
  any platform, and the language setting was read from the wrong column
  on macOS (always empty, so it never applied). Booleans now travel as
  `on`/`off` end to end, and settings-set accepts those words back along
  with the script spellings true/false/1/0.

- **The quick settings rows were nine cells wide.** The row contract is
  eight; hosts that check the width (macOS) silently dropped them, so an
  empty query never showed the theme/hotkey/language rows it promised.
  They are eight cells now, and a wire test asserts the width of every
  row in every query so this class of bug cannot ship again.

- **macOS: a stray "Fulcrum Settings" window appeared on every launch.**
  The About panel rode a SwiftUI Settings scene, and on current macOS
  every SwiftUI window scene presents its window at launch — so an About
  panel sat behind the launcher from the first start. The scene is gone;
  About opens on demand from the menu bar, the app menu and the `about`
  row, in its own window.

- **macOS: the About panel could show the wrong version.** It read the
  bundle's Info.plist, packaging metadata that drifts on a hand-assembled
  bundle; it now shows the version compiled into the binary — the same
  number the backend reports and the updater compares against.

- **The macOS menu bar showed the app name instead of an icon.** The
  status item was installed with a text title, so it read as yet another
  menu instead of a status item. It now carries the brand mark — the
  lever balanced on a wedge over a ground bar — drawn in code as a
  template image so it follows the menu bar's light/dark appearance,
  the same approach as movebit's droplet (SwiftPM resource bundles
  cannot ship inside a signed rivet app bundle).

- **Applications installed after startup never appeared.** The app
  index was a startup snapshot, so a bundle installed later stayed
  invisible until relaunch (the `index-rebuild` RPC existed but nothing
  ever called it). Every search now stats the discovery roots — a
  handful of stat calls, microseconds per keystroke; an installed or
  removed bundle changes a root's mtime and only then does the index
  rebuild inline. Unchanged roots cost the search path nothing. The
  refresh and the explicit rebuild share one fingerprint cache, and the
  `index-rebuild` wire test, parked since an older rivet hung inside
  the serve worker, is wired back in — that hang is gone (fixed by
  rivet's keep-dispatch-live work, verified against both main and the
  v0.11.0 pin).

### Changed

- **macOS: Fulcrum is now a menu bar app — no Dock icon.** The launcher
  convention (Spotlight, Raycast, Alfred): the menu bar glyph and the
  hotkey are the ways in, and opening the app again from Finder,
  Spotlight or `open -a Fulcrum` while it runs summons the panel instead
  of doing nothing. Hiding the panel hands focus straight back to the app
  you came from. The SwiftUI scene layer is gone with it; the app menu
  (About, Check for Updates, ⌘Q), the Edit menu (⌘V paste into the query
  field, ⌘A, ⌘C) and the Window menu (⌘W/⌘M on the About and update
  windows) are hand-built so the keyboard plumbing stays intact.

- **One glass panel across all three platforms.** The launcher now reads
  as the same product everywhere, each host rendered with its own native
  material: Desktop Acrylic on Windows (the window is chromeless — the
  content is the panel), vibrancy on macOS, and an alpha-glass GTK
  stylesheet on Linux (KWin and other blurring compositors frost it;
  without a compositor it degrades to solid). All three share the row
  grammar the macOS panel already had: amber icon tiles, title over a
  muted subtitle, badge chips, an inset rounded selection in the product
  accent (#D97706) instead of system blue/default, and the ↵ affordance
  chip on the selected row only. Windows rows previously showed a bare
  title string and a permanent InfoBar strip; routine status now lives
  quietly in the footer and the InfoBar appears only for things that
  need attention. Linux gains a styled search entry, pointer hover, and
  symbolic icon tiles with theme-checked fallbacks (a bare system never
  paints the missing-icon box).

## [0.12.0] - 2026-10-16

### Added

- **GNOME Alt+Space conflict, detected and one click away from fixed.**
  GNOME's stock *activate window menu* keybinding owns `Alt+Space`, and
  the compositor swallows the summon press before the portal shortcut
  ever fires — the binding is granted, the hotkey just never runs. When
  the portal reports an Alt+Space trigger and that stock binding still
  exists, the Linux host says so in the status bar and grows a **Fix
  Alt+Space conflict** item in the tray menu; running it clears the
  stock binding (`gsettings set org.gnome.desktop.wm.keybindings
  activate-window-menu "[]"`), reports success, and retracts the item.
  Both user guides document the manual remedy. Found live: the first
  full-surface verification pass ended with exactly this diagnosis on
  this machine.

## [0.11.3] - 2026-10-16

### Fixed

- **"Check for updates now" reported a bogus failure on every platform.**
  The run-action branch matched the check result with a `case` over
  symbol literals while `record-ref` hands back strings — an
  `up-to-date` result always fell to the error branch and surfaced as
  `update check failed: #<void>` (the `or` treats the void error field
  as truthy). The successful paths now compare strings, and a real
  failure reports its actual reason instead of `#<void>`. Found by a
  full-surface verification pass on Linux; the silent launch check was
  never affected (it rides events, not this branch).

## [0.11.2] - 2026-10-15

### Added

- **A tray icon on Linux** — the visible way back after the panel hides
  (close button, Esc, focus loss). StatusNotifierItem wherever a watcher
  runs (GNOME with the AppIndicator extension, KDE, wlroots trays): left
  click presents the panel, the menu has Open Fulcrum / Quit Fulcrum in
  the shared i18n source, and the labels follow the backend `language`
  setting without a restart. Sessions without a tray keep the portal
  hotkey and `fulcrum --toggle` as summon paths.

### Fixed

- **SIGTERM no longer half-kills the Linux host.** The shutdown hook was
  wired in the 0.4 era (fulcrum#6), then lost to later scaffold
  rewrites: the embedded Racket runtime absorbed SIGTERM, so `pkill`
  could not stop a resident launcher and half-dead processes piled up.
  The hook is installed again — after the runtime boots, on the main
  thread, where it is the last writer — and SIGTERM/SIGINT now run the
  orderly backend stop and exit(0).
- **The WM close path keeps the window resumable.** The close button now
  routes through an explicit close-request handler that hides the panel;
  letting GTK's hide-on-close default run was observed killing the X11
  surface behind GDK's back, leaving a resident process with a window
  that could never be presented again.

### Changed

- The generated identity constants (`GeneratedBackend.*`, all three
  hosts) ride the release commit again — 0.11.1 shipped with the
  in-tree constants still at 0.11.0, so About panels in source builds
  lagged one release (release-time codegen kept shipped artifacts
  correct).

## [0.11.1] - 2026-10-15

### Fixed

- **The Linux host is no longer an invisible app.** The panel now opens at
  launch: starting hidden was unworkable on any session where the summon
  path wasn't alive yet — Wayland before the portal consent is granted,
  after a reboot, a rejected grab — so a fresh install looked dead (no
  window, dead hotkey, nothing to click). Esc or focus loss still hides
  the panel immediately.
- **`fulcrum --toggle` with nothing running is a summon, not an error.**
  It used to exit with `no running instance to toggle`, which killed the
  documented compositor-keybinding fallback after every reboot; now it
  starts the launcher and opens the panel.
- The portal hotkey path reports the binding the compositor actually
  chose ("Summon hotkey: Alt+Space") in the status bar instead of
  silence; `linux/README.md` now describes the real portal-first hotkey
  and lease-based single instance (both had drifted).

## [0.11.0] - 2026-10-14

Foundation round — the launcher finally answers "what is this and where
do I report it", on every host, and the Windows and Linux hosts learn to
speak the shared language.

### Added

- **About, on all three hosts.** Query `about` in the launcher and the
  backend ranks one `about.open` row (version, website, feedback,
  license); running it delegates host-native — a native About window on
  macOS, a GTK about dialog on Linux, a ContentDialog on Windows. The
  panel is reachable without the row too: macOS grows an "About Fulcrum"
  menu-bar item and its long-empty Settings scene now shows the About
  panel; Windows gains "About Fulcrum" in the tray menu. Every panel
  carries the version line, the site (fulcrum.jrtx.site), the issue
  tracker and "Business Source License 1.1" (macOS strings ride the
  shared/i18n single source — new `about*` keys, zh and en).
- **The Windows and Linux hosts speak the shared i18n source.** Both
  hardcoded the update/about surface in English; they now load the staged
  `shared/i18n` res JSON (dependency-free line-based loader over an
  embedded fallback table of the same keys) and follow the backend
  `language` setting, applied on the next launch — a missing file
  degrades to correct copy, never to key names.
- **GitHub issue templates** (bug report + feature request) so feedback
  from the About links lands structured: the bug form asks for the
  platform pair, install shape and hotkey setup, and never for an AI key.
- **Websearch commands are unit-tested**: bang routing, RFC 3986
  percent-encoding of query metacharacters (a stray `&` or `=` is a
  query injection into the browser URL), and the fallback contract.

### Fixed

- **The backend's headless version fallback drifted**: the `must track
  rivet.rktd` literal in `app/backend.rkt` still said 0.10.0 after
  0.10.1 shipped, so every headless run (tests, dev loops) believed it
  was outdated. It now tracks the release (0.11.0 as of this release),
  and `check-release-version.sh` grew a fourth gate — VERSION ==
  rivet.rktd == backend fallback == tag — so the literal can never
  drift again.

### Changed

- **Update failure text is selectable** in the macOS update panel's
  failed state, so the exact error can be copied into a bug report
  instead of retyped (Windows' status line already had its copy
  affordance; Linux' status label is selectable).
- **One RIVET_PIN per workflow**: the five literal rivet pins in CI and
  release collapse into `${{ env.RIVET_PIN }}`, so a rivet bump is one
  edit per file, not a grep-replace.
- **Versionless stable aliases ride every release**: the primary
  installers are also uploaded as `fulcrum-macos-arm64.dmg` (etc.), so
  `releases/latest/download/<alias>` stays a permanent URL and the site
  hero CTA points straight at the right download with OS/arch detection.
- **Dead code leaves**: `files.rkt`'s unused `search-command` and
  `plugins.rkt`'s `remember-results!` are gone; the release runbook §2
  now matches reality (the update verification key is committed source,
  not swapped per release).

## [0.10.1] - 2026-10-14

Engineering-alignment patch — no feature changes to the launcher
itself. The hosts consume Rivet as a package library (rivet v0.11.0)
instead of compiling its sources, the update rollout bucket draws from
cryptographic randomness, the website joins the family design system
(dark theme, Chinese default), and the site gains a bilingual user
manual.

### Added

- **User manual at fulcrum.jrtx.site/guide.html** — a bilingual
  (Chinese default, English via the shared nav switcher) web manual
  following the family manual pattern: a five-minute start (install →
  summon → first command/snippet → paste back into the frontmost app),
  the core features (search, clipboard history, snippets, AI key setup,
  window & system commands, hotkey customization), and a FAQ covering
  the real stumbling blocks — macOS Accessibility grants, where data
  lives, accounts, platform differences, language & theme, failed
  updates, uninstall. Illustrated with real screenshots of the macOS
  panel in light and dark themes; the site nav gains a "Manual" entry.

### Changed

- **Rivet is consumed as a package, never as compiled-in sources**
  (project constraint, rivet#238). The Linux host builds via
  `add_subdirectory("${RIVET_ROOT}/platform/linux")` + `rivet::platform`
  instead of hand-copying the platform translation-unit list; the
  Windows host links `rivet-host.vcxproj` through a `ProjectReference`
  (CRT flavor forwarded via `RivetRuntimeLibrary`) instead of
  `ClCompile`-ing four Rivet sources; the macOS host depends on the
  rivet root `Package.swift` (`.package(path: rivetRoot)`, products of
  package "rivet") instead of the platform subpackage. CI and release
  workflows pin Rivet to the `v0.11.0` git tag instead of floating on
  the package catalog, and the `setup-embed-racket` action reference
  moves from `@main` to `@v0.11.0`.
- **Website (fulcrum.jrtx.site)** aligns with the family design system:
  copper accent palette (`#c15f3c` on `#faf7f2`), a complete dark theme
  with a manual toggle (localStorage choice, system-preference fallback,
  anti-flash head script), and `data-i18n` dictionaries with Chinese as
  the default language (`lang="zh-CN"`; title and meta description
  switch with the language). The hardcoded version badge is gone — the
  homepage no longer states a version number.

### Fixed

- **Update rollout buckets are cryptographically random.** The sticky
  0..99 bucket assigned on first update check is now derived from
  `crypto-random-bytes` instead of `random` — racket/base's PRNG has a
  fixed seed, which put every fresh install into the same lockstep
  bucket and would have defeated any staged rollout.

## 0.10.0 — October 13, 2026

Snippet auto-expansion on macOS and Windows — the last core Raycast
base feature — plus the post-0.9.0 base-completeness wave.

### Added (snippet auto-expansion)

- Type a snippet keyword in any app, a word boundary fires, and the
  keyword is replaced with the full snippet text in place. macOS uses a
  CGEventTap with unicode key injection; Windows uses a low-level
  keyboard hook with SendInput repair. Toggle in `settings` →
  `expansion-enabled` (macOS requires the accessibility permission).
  Linux is not supported — Wayland does not allow the required input
  monitoring, documented in the user guide.

### Added (system commands)

- **Toggle Dark Mode** on macOS (System Events appearance), Windows
  (theme registry), and GNOME (color-scheme); **Log Out** and **Empty
  Recycle Bin** on Windows; **Empty Trash** on macOS and GNOME.

### Fixed (file search was dead on large result sets)

- **File search never returned results for common queries**: run-command
  waited for the child process to exit while the child blocked writing
  output larger than the 64 KiB pipe buffer — a mutual deadlock that
  timed out every search whose result set was big ("find README" on a
  dev machine). The stdout drain now runs concurrently with the exit
  wait; a search that used to return zero rows in 1.5 s now answers in
  ~180 ms with results. Regression test included.

### Added (emoji search)

- **Emoji search**: type two letters and matching emoji appear inline
  (Raycast-style) — Enter pastes the character straight into the app you
  came from, ⌘K/Ctrl+K copies it. A curated local dataset of ~300
  popular emoji with English keywords; no network, no name-table
  download.

### Added (Linux Wayland hotkey)

- The Linux host registers its summon shortcut through the
  `org.freedesktop.portal.GlobalShortcuts` portal where available
  (Wayland compositors and modern desktops): the compositor asks for
  consent once and owns the final binding. Sessions without the portal
  keep the X11 grab; older Wayland sessions still bind
  `fulcrum --toggle` in compositor settings.

### Added (Raycast 2.0 parity: the AI memory trio)

- **Persistent AI memory**: the conversation transcript and last answer
  survive restarts (ai-memory.json, local-only, never synced — a
  transcript is sensitive)
- **`ai undo`**: drop the last chat exchange and retry from there — the
  minimal branching primitive
- **`ai persona <text…>`**: persistent personalization, delivered as a
  system message on every request

### Added (polish wave)

- **`ai model <name>`**: set the AI model from the launcher — model names
  are free-form, so a settings cycle does not fit; bare `ai model` shows
  the current value
- **Tab completion** on all three hosts: Tab autocompletes the selected
  row's title into the query field (the Raycast convention for refining a
  fuzzy hit into exact text)

### Fixed

- AI chat memory and the last-answer row lived in Racket parameters,
  which are per-thread — every RVT1 request runs on its own worker
  thread, so both silently reset between requests in production while
  the single-threaded tests stayed green. Both now live in module-level
  boxes and actually persist for the backend session

## 0.9.0 — October 12, 2026

The launch-parity wave: closing the gaps a user touches daily, with the
FPP2 core landing alongside.

### Added (FPP2 wave)

- **Plugin protocol v2** ([docs/fpp2.md](docs/fpp2.md)): daemon-mode
  plugins stay warm across queries (lazy spawn, per-op timeouts, crash
  backoff, LRU cap, rate-limited results-changed push), results can
  declare ⌘K actions and a detail view (native text + metadata panel on
  every host), and manifests can declare preferences the backend stores
  and injects — edited with `plugin set <id> <pref> <value…>` or cycled
  from the plugin detail rows. FPP1 is unchanged and permanent.
- **Live results**: a new `search-revision` State makes all three hosts
  refresh the on-screen rows when a backend daemon pushes updates.
- Conformance kit: `tests/fpp2-test.rkt` drives a real daemon fixture
  through warm queries, interleaved ops, action routing, preference
  injection, nudge rate-limiting, and the crash/backoff/cold-fallback
  path.

### Fixed

- Update-check results no longer hide the panel before they can be
  read; the status line is copyable on all three platforms.

The launch-parity wave plus the FPP2 core: closing the gaps a user
touches daily, then the plugin protocol upgrade. The wire contract
moved once; all three hosts ride together.

### Added

- **Paste to frontmost**: clipboard rows now paste straight into the app
  the user came from (Enter pastes; Copy stays in the ⌘K panel), snippets
  and the AI last-answer get a "Paste to frontmost" action. Hosts write
  the pasteboard, hide the panel, and synthesize the platform paste
  keystroke — Accessibility on macOS, SendInput on Windows, XTEST on X11
  and `wtype` on Wayland on Linux
- **Streaming AI**: AI answers stream into the panel as they are
  generated (new `AiStream` State, phases running → done|error) and land
  on the clipboard when finished; running an AI row no longer blocks the
  panel for the whole model call
- **Snippet one-liner creation**: `add snippet <keyword> <text…>` saves a
  snippet from the launcher, mirroring `add link`; query `snippets`
  lists the store
- **Host-rendered hotkey and theme**: the launcher hotkey and UI theme
  settings now reach all three hosts — hosts pull them on startup, follow
  live `$state` pushes (the Carbon/Win32/X11 grab re-binds, the panel
  switches appearance), the settings row cycles through four presets,
  and a taken combo falls back with a status note instead of stranding
  the launcher
- **Linux clipboard capture**: the GTK host records clipboard changes
  (1 s poll), so clipboard history finally exists on Linux
- **User guide**: `docs/user-guide.md` (+ `user-guide.zh-CN.md`),
  written for people who have never used a launcher
- **Design drafts for the next wave**: `docs/fpp2.md` (plugin protocol
  v2 — daemon mode, detail view, result actions, preferences, the
  staged permissions ladder) and `docs/registry.md` (the GitHub-only
  curated plugin registry: signed index on Releases, PR-based
  submission, embedded gallery as the offline baseline)

## 0.8.0

Packaging parity release: the release asset set now matches the family
benchmark (taskly) format-for-format on every platform it ships, and the
update feed moves to the family-standard file name.

### Added

- **Linux arm64 joins the matrix**: the Linux job is a two-architecture
  matrix on native `ubuntu-24.04-arm` runners — tar.gz, deb, rpm, AppImage,
  and portable zip, same recipe as x64
- **Portable zips for Linux and Windows** (`fulcrum-<version>-linux-<arch>.zip`,
  `fulcrum-<version>-windows-x64.zip`): rivet release already produced them
  with their own `.sha256` sidecars; the workflow now hoists them into the
  release instead of dropping them (the duplicate rivet macOS zip is
  discarded — the hand-built zip stays)

### Changed

- The signed update feed is now published as `update-manifest.json` (family
  convention) and carries a Linux arm64 tar.gz entry; a byte-identical
  `manifest.json` rides along so pre-0.8.0 clients polling the old name
  keep updating — the alias will be dropped once 0.7.x installs are gone
  from the field (same transition policy payback used)
- The site version badge tracks the release again (it had drifted at v0.5.0)

## 0.7.0

### Added

- **Native Linux packages**: `raco rivet release` now produces deb, rpm, and
  AppImage beside the signed tar.gz on x64 — the family rivet 0.6.1
  linux-formats mechanism, previously opted out on this line. The deb
  installs `/opt/fulcrum` with a desktop entry and pixmaps icon
  (`Depends: libgtk-4-1`); the rpm builds through rpmbuild
  (`Requires: gtk4`); the AppImage bundles its own GTK4 dependency closure
  with the 512 px brand icon (`assets/fulcrum-icon-512.png`, rendered from
  the site favicon SVG) as the top-level AppDir icon. The update feed
  contract is unchanged: the signed channel manifest still pins the four
  portable artifacts, and Linux updates still flow through the tar.gz —
  the new formats ship as GitHub release assets only.

## 0.6.0

### Added

- **Updater 0.2 — download and install** (family pattern, taskly parity):
  the backend now owns the whole verify-download pipeline. `update-check`
  (manual and silent variants) returns a typed `UpdateCheck` record
  (status/version/build/published-at/installer/size-bytes); `update-download`
  streams the signed artifact to `<data-dir>/updates/` on a background
  thread with progress in a state box; `update-state` reports phase
  (idle | checking | downloading | downloaded | error), percent and the
  verified file path. The 4-hour silent-check throttle is backend-owned
  (settings key `update-last-check`) so all three hosts share it, and the
  staged-rollout bucket persists sticky as `update-rollout-bucket`.
- **Launcher update rows**: query `update` on every platform surfaces the
  one row the current phase wants — check, download (version + size),
  live progress, then "Quit and install" (macOS) or "Open the updates
  folder" with honest manual-install guidance (Windows: the downloaded
  .msi; Linux: the .tar.gz). Windows and Linux hosts additionally poll
  download progress into the status line and run the throttled silent
  check at launch.
- **macOS update window + in-place install**: a menu-bar "Check for
  Updates…" item opens an update panel (available → confirm version + size
  → download progress → quit-and-install). Installation swaps the verified
  bundle atomically (unzip → ditto → keep the old bundle as `.old` until
  the relaunch succeeds, taskly's proven swap script) and relaunches.
- **i18n for the update flow**: `shared/i18n/{zh,en}.json` ship as staged
  resources (`resources` clause in rivet.rktd) and the macOS host renders
  the update panel in the configured language (`language` setting, zh
  default, toggle via the launcher settings rows).

### Fixed

- The updater no longer draws a fresh rollout bucket per check; the bucket
  is drawn once and persisted, matching the family update contract.

## 0.5.0

### Changed

- **Release artifacts now carry the full version and architecture in their
  names** — `fulcrum-0.5.0-macos-arm64.dmg`, `fulcrum-0.5.0-macos-arm64.zip`,
  `fulcrum-0.5.0-windows-x64.msi`, `fulcrum-0.5.0-linux-x64.tar.gz` — instead
  of the fixed `fulcrum-macos.dmg`-style names. Every artifact ships with a
  `.sha256` sidecar, and the publish job assembles them into a combined
  `SHA256SUMS` (family packaging standard).

### Added

- **macOS Intel build**: releases now include an x64 DMG and zip alongside
  the Apple-silicon ones (the taskly-style mac matrix), plus a portable
  `fulcrum-<version>-macos-<arch>.zip` per architecture — the zip is also
  what the signed update feed serves macOS clients.
- **Release version gate**: a `VERSION` file at the repo root and
  `scripts/check-release-version.sh` (VERSION == rivet.rktd == tag) run in
  CI and in every release packaging job.

### Fixed

- The update check actually verifies now: the embedded public key is
  decoded into a key object (`bytes->ed25519-public-key`, evaluated at
  check time — FFI at module-import time kills embedded apps at startup)
  before reaching the manifest verifier. 0.4.x passed raw DER bytes, so
  every update check died on an `ed25519-verify` contract violation and
  surfaced as "update check failed". The release pin moved to the rivet
  snapshot that carries the bytes key parser (2e1924c).
- The update manifest architecture labels now match what the in-app updater
  sends (`arm64`/`x64`, the rivet family vocabulary). The old manifest wrote
  `arm64`/`x64` while the updater sent `aarch64`/`x86_64`, and
  `select-update` matches with `eq?` — so no client ever matched an artifact
  and every update check reported "up to date" even when a newer version had
  shipped. Manifest URLs are also tag-pinned now
  (`releases/download/<tag>/<artifact>`) instead of riding
  `releases/latest/download/`.

## 0.4.2

### Changed

- **Releases now ship one human installer per platform instead of bare
  archives**: `fulcrum-macos.dmg`, `fulcrum-windows-x64.msi` (Start-menu and
  desktop shortcuts included), and `fulcrum-linux-x64.tar.gz`. The release
  pipeline runs `raco rivet release`, and the signed channel manifest pins
  those installers — no more unpacking a WinUI runtime directory to hunt
  for `RivetHost.exe`.

### Fixed

- The update check now actually ships the redirect-following fetch:
  0.4.1 announced this fix but its release pin predated rivet#153, so
  0.4.1 binaries still failed every in-app update check at signature
  verification. 0.4.2 builds on the commit that carries the fix.

## 0.4.1

### Fixed

- The update check follows HTTP redirects (rivet#153): GitHub release
  assets answer with a 302 to their CDN, and the previous fetch verified
  an empty redirect body — every in-app update check failed at signature
  verification. No app changes; rebuilt on the fixed rivet.

## 0.4.0

### Added

- **Online updates go live**: the update channel now serves the signed
  channel manifest straight from GitHub Releases
  (`releases/latest/download/manifest.json`) — no separate downloads origin
  to operate. The default `update-base-url` setting points there, and the
  updater fetches `/manifest.json` under whichever base is configured.
  Checks stay signature-verified (Ed25519, key `fulcrum-2026-10`) and
  honest: a developer build reports "developer build", a failed origin
  reports the error verbatim.

## 0.3.0

### Added

- **First-party plugin gallery** — ten plugins built into the app, listed
  by querying `gallery` or `plugins`, installed and uninstalled from the
  launcher itself (each install row is an engine action; no new UI surface
  on any host, and installed plugins are queryable without a restart).
  Fuzzy queries suggest installs for missing plugins the way apps are
  suggested. The gallery can only remove plugins it installed (a marker
  file guards user-installed ones), and wire-supplied plugin ids are
  validated against a strict pattern before touching the filesystem.
  The opening roster: Epoch (`ts`), Unit Converter (`u`), Regex (`re`),
  Timezone (`tz`), GitHub (`gh`), Cargo (`crate`), Homebrew (`brew`),
  Docker (`dk`), winget (`winget`), Bitwarden (`bw`) — every tool-backed
  plugin degrades to a single explanatory row when its CLI or network is
  unavailable.
- **Sync beta** for settings and snippets: point the new `sync-root`
  setting (or the `FULCRUM_SYNC_DIR` environment variable, which also
  bootstraps a wiped machine) at any directory a file sync service
  replicates; every write mirrors there and a newer mirror restores the
  local file at startup — that is the whole wipe-and-restore story.
  Conflict policy is newest-file-wins by mtime; clipboard history and
  recents are deliberately not synced.
- **Currency plugin** (the eleventh first-party) — `cur 100 usd in eur`
  against the keyless open.er-api.com daily feed, with the usual
  graceful-degradation row when the network is down.
- **Quicklinks** — user-defined URL templates claimed by a keyword, the
  cheap half of Raycast's quicklinks: `yt nature` opens the YouTube
  search for "nature"; a URL without `{query}` is a plain bookmark.
  Created from the launcher itself with a one-line grammar
  (`add link yt https://youtube.com/?q={query} YouTube`), listed and
  deleted under `quicklinks`, and synced with settings and snippets.
- **Window management** — Left Half, Right Half, Maximize, Almost
  Maximize, Center and Restore for the frontmost window. The engine
  ranks the rows; each host executes natively (Accessibility API on
  macOS, which honestly reports the missing permission until granted;
  `SetWindowPos` on Windows; EWMH `_NET_MOVERESIZE_WINDOW` on
  Linux/X11, compile-tested like the rest of that host). A new
  `delegated` action status carries the backend's verdict to the host.
- **BYOK AI** (the roadmap's phase-1 shape: the user's key, our cost
  zero): `ai summarize`, `ai clean`, `ai translate <lang>`, `ai explain`,
  and bare `ai <question>` — with the clipboard attached as context where
  it makes sense. Rows appear instantly in search; running one calls the
  provider (OpenAI, Anthropic, or a local Ollama endpoint) and the answer
  copies to the clipboard, so a slow model never traps the user in the
  panel. The API key lives in `ai-keys.json` inside the data directory
  and is deliberately excluded from sync; `ai key <secret>` saves it from
  the launcher, `settings → ai-provider` (now a cycle in the settings
  rows) picks openai / anthropic / ollama. Unconfigured installs show
  honest setup rows instead of pretending.
  AI is a first-class citizen now: **`ai chat <msg>` holds a
  conversation** (session history threaded into every request, capped at
  eight exchanges, `ai chat reset` clears), **any typed question** —
  `what is entropy?`-style, configured installs only — offers an
  "Ask AI" row without the prefix, and the **last answer stays visible**
  under `ai` as a one-keystroke copy instead of vanishing into the
  clipboard.
- **File search** — `find <query>` over Spotlight (`mdfind -name`,
  deduped, capped at 8 rows); a result opens with the platform opener.
  Linux and Windows get one honest row instead of a pretend search until
  their index providers land.
- **Settings in the launcher** — the hosts have no settings window, so
  the launcher is the settings UI: query `settings` lists theme,
  max-results and the clipboard/web/plugin toggles with their current
  values as badges; running a row cycles or toggles the value and
  notifies the new one.
- **Plugin center** — the launcher is the management surface: query
  `plugins` for every installed plugin with an Enabled/Disabled toggle
  (persisted, honored by the loader on reload), a row that reveals the
  plugins directory, and the gallery's install/uninstall rows underneath.
  `install plugin <path>` copies a third-party plugin folder into place —
  the destination comes from the folder's manifest id (never the folder
  name, so a spoofed name cannot escape the plugins directory) — and
  `plugins.uninstall` removes any plugin, gallery-owned or user-installed.
- **Marketplace-grade plugin center** — first-party manifests now carry
  author, category, and per-command usage examples. `plugins <name>`
  expands a detail view: every command with a **copyable example**
  (select → the example sits on the clipboard, paste into a fresh query
  to try it), the declared permissions by name (or an honest "none
  declared"), and the category. The **update flow** exists at last: when
  the app ships newer plugin code than an installed copy, the listing
  offers "Update <name> → v<new>" and overwrites in place (gallery-owned
  plugins only; user data never lives in plugin directories).
- Gallery, sync, quicklinks, AI, file search and the plugin center carry
  21 new backend tests (69 total).

### Changed

- The epoch reference plugin moved from `examples/plugins/` to
  `gallery/epoch` — the gallery is now the single home of first-party
  plugins, and `scripts/gen-gallery.rkt` embeds their files into the
  compiled backend (CI regenerates and fails on drift).
- The backend now reads its version from the staged
  `rivet-app-info.rktd` (`rivet/app-info`) instead of a hardcoded string,
  with the rivet.rktd version as the headless fallback. 0.2.0 shipped
  reporting "0.1.0", which would have made every install believe an
  update was available once the channel manifest went live.

### Changed

- **Menu bar / tray presence** (the discoverable way in, and the only
  quit affordance since Esc just hides): macOS installs the first-party
  rivet menu bar with "Open Fulcrum" and "Quit Fulcrum"; Windows rides
  the first-party Shell_NotifyIcon tray surface with the same two
  actions (left click toggles the launcher, right click opens the
  menu). Linux is deferred honestly — the rivet tray contract is
  deliberately absent there (StatusNotifierItem, upstream #118).
- All hosts: **⌘K action panel** — Ctrl+K (Win/Linux) and ⌘K (macOS)
  open the selected row's secondary actions: Reveal in Finder and Copy
  path for apps, Pin/Unpin and Delete for clipboard entries, Delete for
  snippets, Copy URL/path for links, files and web rows. Esc walks back
  to the live search; mutating actions keep the launcher open instead of
  dismissing it.
- All hosts: **Esc follows the Alfred/Raycast convention** — it clears
  the query first and hides the panel only when the query is already
  empty, so a stray Esc never throws away typed text. The app itself
  never quits: the panel hides, the single instance (and the clipboard
  watcher) stay resident.

### Fixed

- macOS host: result row identity was `(action id, arg)` — unique for
  engine providers, but FPP1 plugins legitimately return several rows
  sharing one arg (Epoch renders a timestamp as UTC *and* local). SwiftUI
  collapsed them to one row; the row title now participates in the
  identity. Found on the first real-machine gallery smoke.

## 0.2.0

### Changed

- macOS host: Raycast-grade panel — under-window vibrancy with rounded
  corners and a hairline edge, real dock icons for application rows (tuned
  SF Symbols for calculator/clipboard/snippet/web/plugin/system rows),
  inset amber selection highlight with a ↵ affordance chip on the selected
  row, and tightened typography and spacing. The brand amber (#D97706)
  matches the product site; both appearance modes verified on screen.
- All hosts: single-instance activation now rides Rivet's first-party
  system services — `RivetSingleInstance` on macOS, `SingleInstanceLease`
  on Windows and Linux — replacing the hand-rolled pid-file + SIGUSR1
  scheme on Linux (the `fulcrum --toggle` contract is unchanged; the
  activation handler hops to the GTK main loop) and adding the previously
  missing guard on macOS and Windows, where a second launch silently grew
  a second panel.
- Plugins: the FPP1 spawn path resolves interpreter names on PATH like a
  shell would. A manifest entry of `"python3"` forked a doomed child
  (Racket's `subprocess` does no PATH lookup), the failure vanished into
  an empty result list, and the reference plugin never appeared in
  searches. Spawn failures now surface as plugin errors instead.

### Fixed

- End-to-end verification on macOS surfaced launch-blocking backend bugs
  that CI never reached, because every discovery-path test exercised only
  the Linux branch:
  - `macos-apps-from-dirs` passed a `#:stop` keyword to `in-directory`,
    which takes its descend? predicate positionally — the backend died
    during `engine-rebuild-index!` on every real launch.
  - `application-id-from-path` formatted a hash in base 36;
    `number->string` accepts only 2/8/10/16, so macOS and Windows
    discovery crashed the same way.
  - `spawn-command`/`run-command` passed bare program names to Racket's
    `subprocess`, which performs no PATH lookup: the child forked, exec
    failed, and the helper reported success. Launching applications
    (`open`) and the `plutil` bundle-name probe never actually worked;
    the probe was masked by the directory-name fallback. Program names
    are now resolved on PATH (or rejected) in `app/core/proc.rkt`.
- macOS host: result rows carried the *action* id (`app.launch`) as their
  SwiftUI identity, so the panel rendered one row a dozen times. Row
  identity is now `(action id, arg)`, unique per row; the action id still
  drives `run-action`.
- macOS host: backend events were parsed as lists, but RVT1 event frames
  hand the host a bare string payload — copy-to-clipboard, open-url and
  notify were silent no-ops. The calculator's ↵-to-copy now works.
- macOS host: the Esc/↑↓ key handler was a background view outside the
  field editor's responder chain and never fired; replaced with a window
  local `NSEvent` monitor.
- Windows and Linux hosts: the same event-payload bug as the macOS host —
  they parsed event values as lists, but the runtime delivers the bare
  string after unwrapping the RVT1 `[name, value]` frame, so
  copy-to-clipboard, open-url and update-available were silent no-ops.
- Linux host: search completions carried no generation guard (both other
  hosts have one), so two overlapping searches could let a stale result
  set overwrite the newer one.
- macOS (Rivet): `raco rivet build` staging invalidated the embedded
  framework's signature via `install_name_tool` and never re-signed it,
  so AMFI killed every `raco rivet dev` launch with "Code Signature
  Invalid". Staging now ad-hoc re-signs the inner dylib and the wrapper;
  `package` re-signs with the release identity as before.

### Tests

- 39 → 43 backend tests: macOS and Windows application discovery from
  fixture directories (plist bundle name with graceful fallback, no
  descent into nested `.app`, shortcut file filtering), and PATH
  resolution plus missing-binary behavior for
  `run-command`/`spawn-command`.

## 0.1.0

### Changed

- Migrate the Linux host onto Rivet 0.3's official Linux support: the project now uses the standard `linux/` host directory, codegen emits the `rivet::linux_runtime` client for the Linux host, and CI builds through `raco rivet build` with an embeddable Racket CS 9.3 — replacing the hand-rolled CMake bridge. Launcher specifics (X11 global hotkey, EWMH overlay chrome, single-instance `--toggle`) ride on top of the official runtime and startup model.

### Added

- Shared Racket backend (`app/backend.rkt`) over RVT1: `search`,
  `run-action`, `index-rebuild`, `clipboard-record/history/clear`,
  `snippet-list/save/delete`, `settings-list/set`, `plugins-list/reload`,
  `update-check`; States `theme`, `hotkey`, `version`; Events
  `copy-to-clipboard`, `open-url`, `notify`, `update-available`.
- Search engine (`app/core/engine.rkt`) with weighted multi-field fuzzy
  ranking (app metadata, snippet names/keywords/text, clipboard content),
  recents boost with persistence, and a single 8-column result row contract
  shared by every host.
- Calculator provider: self-contained recursive-descent parser — no `eval`
  — with exact arithmetic, right-associative `^`, postfix factorial,
  scientific notation, 18 functions, 4 constants, and every failure
  converted to a one-line `exn:calc`.
- Application discovery per platform: `.app` bundles (macOS, CFBundleName
  via plutil), XDG `.desktop` entries with NoDisplay/Hidden/Type filtering
  (Linux), Start Menu `.lnk/.url/.exe` (Windows).
- Clipboard history store: dedupe-by-content move-to-front, pinning that
  survives the size limit, atomic JSON persistence, bounded entries.
- Snippets store: CRUD by id, keyword lookup, fuzzy search, atomic
  persistence.
- Web-search provider: 8 bangs plus a default-engine fallback row; strict
  RFC 3986 percent-encoding; host-side open restricted to http(s).
- System commands: lock/sleep/restart per platform with availability
  probes.
- **FPP1 plugin protocol** (`app/core/plugins.rkt`, spec in
  `docs/plugins.md`): external process per query/run, one JSON object per
  line, per-call timeout, manifest validation, relative-path entry
  resolution, load-error surfacing. Reference plugin
  `examples/plugins/epoch` (Python).
- Settings manager over `rivet/system`: 10 typed keys, stale-value
  fallback, loud invalid writes.
- Update wiring over `rivet/distribution`: Ed25519-verified channel
  manifest checks; developer builds honestly report "updates unavailable"
  until a key is embedded (runbook §2).
- macOS host: SwiftUI floating panel, nonactivating `NSPanel`, Carbon
  global hotkey (⌥Space with ⌃⌥Space fallback), pasteboard watcher,
  debounced generation-guarded search.
- Windows host: WinUI 3 topmost overlay, `RegisterHotKey` (Alt+Space with
  Ctrl+Alt+Space fallback), `WM_CLIPBOARDUPDATE` watcher, window-subclass
  message pump, stale-completion-guarded search.
- Linux host (experimental): GTK4 panel, X11 `XGrabKey` hotkey with an
  honest Wayland message (`fulcrum --toggle` for compositor keybindings),
  embedded Racket CS over a socketpair through Rivet's shared RVT1 codec.
- CI matrix (backend tests on all three OSes, host compile smokes, FPP1
  fixture end-to-end) and a tag-driven release pipeline with
  tag/version/changelog validation, dev/production packaging, Ed25519
  update-manifest signing, and draft GitHub releases.
- Product site (`site/`, fulcrum.jrtx.site, bilingual), business plan
  (`docs/business.md` + 中文), release runbook, BUSL-1.1 licensing with an
  MIT plugin protocol.

### Tests

- 39 backend tests: fuzzy ranking anchors, calculator surface and hostile
  input, clipboard dedupe/pin/limit/persistence, snippet CRUD, FPP1
  end-to-end with real subprocesses (including a hanging-plugin timeout
  case), desktop-entry parsing, engine ranking/actions/events, settings
  round-trips and stale-value fallback.
