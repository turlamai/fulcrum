#lang racket/base

;; Fulcrum update wiring over `rivet/distribution`.
;;
;; Trust model (inherited from Rivet): HTTPS protects transport but is not
;; the root of update trust. The channel manifest is Ed25519-signed and
;; verified against the public key embedded below before any version
;; decision. Key management lives in docs/release-runbook.md: key
;; generation, the committed public half (`current-update-public-key-hex`,
;; updated only for rotation), and rotation via `current-update-key-id`
;; (ship a client trusting the next key before signing with it).
;;
;; The family pattern: the backend verifies and downloads the signed update
;; artifact through rivet's managed updater (candidate selection, download
;; worker, watchdog, cancellation, terminal state); the native host owns
;; installation. Progress is published to a state box that the UI polls
;; through the `update-state` RPC (RVT1 events are thread-local, so a
;; background thread cannot emit them directly). The feed serves installers —
;; macOS zip (portable bundle), Windows MSI, and Linux tar.gz + AppImage — per
;; .github/sign-manifest.rkt; the DMG is the human installer, not a feed
;; artifact. Linux AppImage installs select the AppImage entry explicitly;
;; existing portable/package-manager installs retain the legacy tar.gz path.
;;
;; Fulcrum-specific rules: the 4-hour silent-check throttle and the sticky
;; rollout bucket live in the typed settings store (keys `update-last-check`
;; and `update-rollout-bucket`, managed by the backend); downloads land under
;; <data-dir>/updates and no store file is ever touched. `verified-install-metadata`
;; is the host handoff ticket built from the authenticated candidate, never
;; from a UI-supplied path or digest.

(require crypto
         net/uri-codec
         net/url
         rivet/distribution
         racket/file
         racket/path
         racket/string
         "core/paths.rkt"
         "core/settings.rkt")

(provide current-update-version
         current-update-settings
         default-update-base-url
         maximum-download-bytes
         update-key-id
         fulcrum-update-configured?
         platform-symbol
         architecture-symbol
         artifact-file-name
         destination-path
         update-state-snapshot
         reset-update-state!
         set-update-error!
         pending-artifact-size
         appimage-self-update?
         package-managed-linux?
         verified-install-metadata
         updates-folder-uri
         perform-check!
         start-download!
         rollout-bucket)

;; Ed25519 public key (DER SPKI), hex-encoded — committed on purpose: the
;; public half only enables verification. Rotation replaces this hex and the
;; key id below (ship clients trusting the next key before signing with it).
;; Key fingerprint: sha256(DER)[:16] = 60b7ce3b8a255465; private half lives in
;; the keys vault and the RIVET_UPDATE_PRIVATE_KEY repository secret.
(define current-update-public-key-hex
  "302a300506032b65700321004797203e4e1fce109e45d41ba755e87eb7d261c082b5bf6728ae0eaaa58e55d7")
(define update-key-id "fulcrum-2026-10")

;; The running version: the packaged app reads rivet-app-info.rktd, so the
;; backend owns the value and hands it over through this parameter. Headless
;; tests parameterize it directly.
(define current-update-version (make-parameter "0.0.0"))

;; The typed settings store, for the check throttle and the sticky rollout
;; bucket. #f (headless tests) degrades to a fresh random bucket and no
;; persisted throttle — the backend always installs a real manager.
(define current-update-settings (make-parameter #f))

;; Where the updater looks for the signed manifest. The release pipeline
;; pins artifact URLs to the concrete tag; the manifest itself is always
;; fetched from the moving "latest" location. Tests parameterize the
;; update-base-url setting at an unreachable local URL instead of touching
;; the network.
(define default-update-base-url
  "https://github.com/turlamai/fulcrum/releases/latest/download")

(define maximum-download-bytes (* 512 1024 1024))

;; ---------- public key ----------

(define (hex->bytes s)
  (let ([n (string-length s)])
    (if (and (even? n) (>= n 2))
        (apply bytes (for/list ([i (in-range 0 n 2)])
                       (string->number (substring s i (+ i 2)) 16)))
        #f)))

(define (fulcrum-update-configured?)
  (and current-update-public-key-hex
       (hex->bytes current-update-public-key-hex)
       #t))

;; ---------- platform identity ----------

;; platform-symbol and architecture-symbol come from `rivet/distribution` —
;; rivet centralized the release-target vocabulary the release tooling
;; emits into manifests. The provide list above re-exports the imported
;; bindings; carrying local copies would collide with the shared names in
;; any module requiring both.

;; The feed serves installers, and the release pipeline pins versioned,
;; architecture-suffixed names (fulcrum-<version>-macos-arm64.zip,
;; -windows-x64.msi, -linux-x64.tar.gz, -linux-x86_64.AppImage) — the signed
;; manifest carries the full URL, so the on-disk name is simply the URL's last
;; segment. Keeping the release name means every host points at exactly the
;; file the manifest vouches for.
(define (artifact-file-name candidate)
  (define raw (update-artifact-url (update-candidate-artifact candidate)))
  (or (and (string? raw)
           (with-handlers ([exn:fail? (lambda (_) #f)])
             (url-file-name (string->url raw))))
      "fulcrum-update"))

(define (url-file-name url)
  (let loop ([segments (url-path url)] [seen #f])
    (cond
      [(null? segments) seen]
      [else
       (define raw (path/param-path (car segments)))
       (loop (cdr segments)
             (if (and (string? raw) (not (string=? raw "")))
                 raw
                 seen))])))

;; ---------- shared update state (UI-visible) ----------

;; phase: idle | checking | downloading | downloaded | error
(define (idle-snapshot)
  (hasheq 'phase "idle"
          'percent 0
          'message #f
          'downloadedPath #f
          'availableVersion #f))

(define update-state (box (idle-snapshot)))
(define updater-box (box #f))
(define download-root-box (box #f))

(define (state-set! key value)
  (set-box! update-state (hash-set (unbox update-state) key value)))

;; The managed phases include transport/selection outcomes the UI treats as
;; idle: `available` carries its signal in availableVersion, `up-to-date`
;; means there is nothing to do, and `cancelled` returns the controls to
;; idle. Anything else passes through unchanged.
(define (managed-state->product state)
  (define phase (hash-ref state 'phase "idle"))
  (define mapped-phase
    (case phase
      [("available" "up-to-date" "cancelled") "idle"]
      [else phase]))
  (define idle? (string=? mapped-phase "idle"))
  (hasheq 'phase mapped-phase
          'percent (if idle? 0 (hash-ref state 'percent 0))
          'message (let ([message (and (not idle?)
                                       (hash-ref state 'message #f))])
                     (if message message #f))
          'downloadedPath (let ([path (hash-ref state 'downloadedPath #f)])
                            (if path path #f))
          'availableVersion (let ([version (hash-ref state 'availableVersion #f)])
                              (if version version #f))))

(define (update-state-snapshot)
  (define product (unbox update-state))
  (define instance (unbox updater-box))
  (cond
    [(not instance) product]
    ;; A backend-side start failure (already running, no candidate)
    ;; outranks the managed snapshot until the next check clears it.
    [(equal? (hash-ref product 'phase "idle") "error") product]
    [else (managed-state->product (updater-state instance))]))

(define (reset-update-state!)
  ;; Dropping an instance while its worker runs would orphan the download;
  ;; land it on the terminal cancelled phase first.
  (define instance (unbox updater-box))
  (when (and instance (equal? (hash-ref (updater-state instance) 'phase)
                              "downloading"))
    (updater-cancel! instance)
    (updater-wait instance 5.0))
  (set-box! updater-box #f)
  (set-box! download-root-box #f)
  (set-box! update-state (idle-snapshot)))

;; Pre-spawn download failures (already running, no candidate) surface
;; through the state instead of an RPC error, so host UIs have a single
;; failure channel.
(define (set-update-error! message)
  (state-set! 'phase "error")
  (state-set! 'message message))

;; The authenticated candidate from the managed updater; #f before a check
;; selects one. It survives a failed/cancelled download for an explicit
;; retry and a new check clears it first.
(define (pending-candidate)
  (define instance (unbox updater-box))
  (and instance (updater-candidate instance)))

;; Byte size of the pending candidate's artifact, for row subtitles.
(define (pending-artifact-size)
  (define candidate (pending-candidate))
  (and candidate
       (update-artifact-size (update-candidate-artifact candidate))))

;; Host handoff ticket: never trust a path or digest supplied by the UI.
;; The ticket comes exclusively from the currently selected, signed manifest
;; candidate after the backend completed size/SHA-256 verification and
;; atomically renamed its .partial file to the downloadable final path. Only
;; expose it while the verified download is committed; a fresh update check
;; resets it.
(define (appimage-self-update?)
  (and (eq? (platform-symbol) 'linux)
       (eq? (current-install-kind) 'appimage)))

;; DEB/RPM (normally under /opt or /usr) are owned by dpkg/rpm and their
;; repository/policy machinery. They must never be replaced by unpacking a
;; tar.gz nor by running the standalone AppImage swap helper.
(define (package-managed-linux?)
  (and (eq? (platform-symbol) 'linux)
       (eq? (current-install-kind) 'package-manager)))

(define (verified-install-metadata)
  (define snapshot (update-state-snapshot))
  (define candidate (pending-candidate))
  (and candidate
       (string=? (hash-ref snapshot 'phase "idle") "downloaded")
       (let ([path (hash-ref snapshot 'downloadedPath #f)])
         (and (string? path)
              (file-exists? path)
              (let ([artifact (update-candidate-artifact candidate)]
                    [manifest (update-candidate-manifest candidate)])
                (and (equal? (simple-form-path path)
                             (simple-form-path
                              (destination-path (data-dir) candidate)))
                     (hasheq
                      'sha256 (update-artifact-sha256 artifact)
                      'rollbackAllowed (update-manifest-rollback-allowed? manifest)
                      'installer (symbol->string
                                  (update-artifact-installer artifact)))))))))

;; The updates directory as a file:// URI the hosts can shell-open (Finder,
;; Explorer, and xdg-open all speak it).
(define (updates-folder-uri)
  (define path (path->string (simple-form-path
                              (build-path (data-dir) "updates"))))
  (define parts (regexp-split #rx"[\\/]" path))
  (string-append
   "file:///"
   (string-join
    (for/list ([part (in-list parts)] #:unless (string=? part ""))
      (uri-path-segment-encode part))
    "/")))

;; ---------- rollout bucket ----------

;; Stable random 0..99 assigned on first check so staged rollouts are
;; sticky per installation. Persisted in the typed settings store;
;; crypto-random-bytes, not `random`: racket/base's PRNG has a fixed seed,
;; which would put every fresh install in the same lockstep bucket.
(define (rollout-bucket)
  (define manager (current-update-settings))
  (if manager
      (let ([existing (settings-get manager 'update-rollout-bucket)])
        (if (exact-integer? existing)
            existing
            (let ([bucket
                   (modulo (integer-bytes->integer (crypto-random-bytes 4) #t #t)
                           100)])
              (settings-set! manager 'update-rollout-bucket bucket)
              bucket)))
      ;; Without a store (headless tests) a fresh draw is honest enough —
      ;; the release channel manifest currently rolls out to 100.
      (modulo (integer-bytes->integer (crypto-random-bytes 4) #t #t) 100)))

;; ---------- install kind ----------

;; AppImage is the one Linux install shape Rivet can update atomically in
;; place. The release feed also retains tar.gz for the existing
;; portable/manual flow, so only an actual AppImage process opts into the
;; AppImage artifact. deb/rpm ownership stays with the package manager and
;; non-AppImage installs keep the existing selection path.
(define (update-install-kind)
  (and (eq? (platform-symbol) 'linux)
       (eq? (current-install-kind) 'appimage)
       'appimage))

;; ---------- managed updater ----------

(define (manifest-url base-url)
  (string-append (regexp-replace* #rx"/+$" base-url "") "/update-manifest.json"))

(define (make-fulcrum-updater base-url)
  (make-updater
   #:application-id "site.jrtx.fulcrum"
   #:current-version (current-update-version)
   #:channel 'stable
   #:platform (platform-symbol)
   ;; Family architecture vocabulary (rivet's own release flow and the
   ;; signed manifest both speak arm64/x64); a mismatch selects nothing.
   #:architecture (architecture-symbol)
   #:install-kind (update-install-kind)
   ;; The feed carries exactly one installable per platform/arch — no
   ;; component discriminator.
   #:component #f
   ;; DER bytes: decoded lazily during the check, so a bad key surfaces as
   ;; an honest "update check failed" instead of killing module load.
   #:public-key (hex->bytes current-update-public-key-hex)
   #:key-id update-key-id
   #:manifest-url (manifest-url base-url)
   #:data-directory (data-dir)
   #:destination
   (lambda (candidate)
     (destination-path (or (unbox download-root-box) (data-dir)) candidate))
   #:rollout-bucket (rollout-bucket)
   #:maximum-manifest-bytes (* 4 1024 1024)
   #:maximum-download-bytes maximum-download-bytes))

;; ---------- check ----------

(define (download-running?)
  (define instance (unbox updater-box))
  (and instance
       (equal? (hash-ref (updater-state instance) 'phase) "downloading")))

;; Returns a plain hasheq describing the outcome; the backend maps it onto
;; the typed UpdateCheck record. Throttling is the backend's job (the
;; update-check RPC persists `update-last-check`).
(define (perform-check! base-url)
  (cond
    ;; Keep the running transfer (and its instance) instead of orphaning
    ;; the worker; the UI can retry the check once the download ends.
    [(download-running?)
     (hasheq 'status "error"
             'message "an update download is already running")]
    [else
     (reset-update-state!)
     (cond
       [(not (fulcrum-update-configured?))
        (hasheq 'status "error"
                'message "updates unavailable: developer build (no update key configured)")]
       [(or (not base-url) (string=? (string-trim base-url) ""))
        (hasheq 'status "error"
                'message "updates unavailable: no update base URL configured")]
       [else
        (with-handlers
            ([exn:fail?
              (lambda (e)
                (state-set! 'phase "error")
                (state-set! 'message (exn-message e))
                (hasheq 'status "error" 'message (exn-message e)))])
          (define instance (make-fulcrum-updater base-url))
          (set-box! updater-box instance)
          (updater-check! instance))])]))

;; ---------- download ----------

(define (destination-path data-dir candidate)
  (build-path data-dir "updates" (artifact-file-name candidate)))

;; Runs on a backend worker thread; the host follows progress via the
;; update-state RPC. Failures surface through the phase.
(define (start-download! data-dir)
  (define instance (unbox updater-box))
  (unless (and instance (pending-candidate))
    (error 'start-download! "no update is available; run a check first"))
  (set-box! download-root-box data-dir)
  (updater-download! instance))
