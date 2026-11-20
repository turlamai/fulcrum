#lang racket/base

;; Progressive local filename catalog.
;;
;; The scanner is intentionally conservative: production defaults cover the
;; three common user-facing folders, never an unbounded home/AppData/Library
;; traversal. Platform indexes continue to provide broader candidates while
;; this local catalog makes common files independently searchable.

(require racket/bool
         racket/file
         racket/list
         racket/path
         racket/string
         "file-index.rkt")

(provide (struct-out file-catalog-status)
         (struct-out file-catalog-worker)
         default-file-catalog-roots
         catalog-root-contains?
         ignored-directory?
         start-file-catalog!
         stop-file-catalog!
         file-catalog-current-status
         file-catalog-wait
         reconcile-directory-in-catalog!)

(struct file-catalog-status
  (state indexed roots-completed roots-total error)
  #:transparent)
(struct file-catalog-worker (thread cancel status root-generations index-path)
  #:transparent)

;; Dependency/tool noise and volume metadata: excluded at every depth on
;; every platform that has them.
(define ignored-directory-names
  '(".git" ".hg" ".svn" ".cache" ".gradle" ".idea"
    "node_modules" "__pycache__" ".venv" "venv"
    ;; Volume system metadata: pure noise on every platform that has it.
    "$recycle.bin" "system volume information"
    ;; macOS noise on volumes a user might add as a root.
    ".spotlight-v100" ".trashes" ".fseventsd" ".documentrevisions-v100"))

;; OS-managed Windows trees. These names are noise ONLY at the system
;; drive's root: a directory called "windows" or "users" on another drive
;; or at any depth is user content (the WinUI host source tree lives in
;; <repo>/windows, acceptance fixtures in …/acceptance/windows). The old
;; name-only match silently dropped every such tree from `find` — on one
;; real machine 43k source files on D: and 218k fixture files on C:.
;; Their system-drive-root content stays reachable through SYSTEMINDEX.
(define windows-os-directory-names
  '("windows" "program files" "program files (x86)" "programdata"
    "perflogs" "users"))

;; True when raw is exactly one component below a lettered drive root
;; (X:\name). Classify on the raw string form so Windows-style paths
;; behave identically on every build platform (see drive-letter-of).
(define (drive-root-child? raw)
  (regexp-match? #rx"^[A-Za-z]:[\\\\/][^\\\\/]+$" raw))

(define (system-drive-root-child? raw system-drive)
  (and (drive-root-child? raw)
       (let ([letter (drive-letter-of raw)]
             [system-letter (and (string? system-drive)
                                 (drive-letter-of system-drive))])
         ;; Without a SystemDrive value keep the conservative drive-root
         ;; guard; with one, only the system drive's root children match.
         (or (not system-letter) (string=? letter system-letter)))))

(define (ignored-directory? path
                            #:system-drive [system-drive (getenv "SystemDrive")])
  (define raw (if (string? path) path (path->string path)))
  (define pieces (regexp-split #px"[/\\\\]+" raw))
  (define name (and (pair? pieces) (last pieces)))
  (and name
       (not (string=? name ""))
       (let ([folded (string-foldcase name)])
         (or (and (member folded ignored-directory-names) #t)
             (and (member folded windows-os-directory-names)
                  (system-drive-root-child? raw system-drive))))))

(define (existing-directory path)
  (and (directory-exists? path) (simple-form-path path)))

;; Classify on the raw string form: Windows drive roots are "C:\"-style
;; strings, and routing them through simple-form-path on other platforms
;; would resolve them as relative paths and break the letter match.
(define (drive-letter-of path)
  (define raw (if (string? path) path (path->string path)))
  (define match (regexp-match #rx"^([A-Za-z]):" raw))
  (and match (string-upcase (cadr match))))

;; Windows data volumes become full-depth catalog roots, so `find` covers
;; every lettered partition instead of only the user folders the OS index
;; reaches (issue #116: a probe file in C:\FulcrumTest or on D:\ was
;; unfindable while macOS Spotlight and Linux plocate cover whole disks).
;; The system drive stays out — its Windows/Program Files trees are pure
;; noise the walker would grind through, and its user content is already
;; covered by the home roots plus the SYSTEMINDEX stage. Volumes whose
;; listing fails (empty CD, disconnected share) surface as honest partial
;; roots through the normal scan path.
;; Every lettered fixed drive becomes a full-depth catalog root — including
;; the system drive. Excluding C: left its user-created directories
;; (C:\tools, C:\projects, anything outside known folders) unfindable, the
;; exact gap issue #116 exists for. The walker's ignore list carries the
;; cost instead: OS-managed trees (Windows, Program Files, ProgramData,
;; Users, PerfLogs) are never descended AT THE SYSTEM DRIVE'S ROOT, so the
;; system drive contributes its top-level files and user-created trees at
;; the same full-depth semantics as data volumes. Same-named directories
;; elsewhere (a repo's windows/ host tree, D:\Users, …) are user content
;; and stay indexed.
(define (data-volume-roots
         #:os [os (system-type 'os)]
         #:all-roots [all-roots
                      (with-handlers ([exn:fail? (lambda (_) '())])
                        (filesystem-root-list))])
  (if (not (eq? os 'windows))
      '()
      (remove-duplicates
       (for/list ([root (in-list all-roots)]
                  #:when (drive-letter-of root))
         ;; Keep roots as paths for the catalog pipeline; on non-Windows
         ;; dev/test hosts the injected letter strings survive as paths
         ;; whose string form still carries the drive prefix.
         (if (string? root) (string->path root) root))
       string=? #:key path->string)))

(define (xdg-user-roots home env)
  (define config-home
    (let ([raw (env "XDG_CONFIG_HOME")])
      (if (and (string? raw) (not (string=? raw "")))
          (string->path raw)
          (build-path home ".config"))))
  (define config (build-path config-home "user-dirs.dirs"))
  (with-handlers ([exn:fail:filesystem? (lambda (_) '())])
    (call-with-input-file
     config
     (lambda (in)
       (for/list ([line (in-lines in)]
                  #:do [(define match
                          (regexp-match
                           #px"^XDG_(?:DESKTOP|DOCUMENTS|DOWNLOAD)_DIR=\"([^\"]*)\"$"
                           line))]
                  #:when match
                  #:do [(define raw (cadr match))
                        (define expanded
                          (if (string-prefix? raw "$HOME/")
                              (build-path home (substring raw 6))
                              (string->path raw)))]
                  #:when (existing-directory expanded))
         (simple-form-path expanded))))))

(define (default-file-catalog-roots
         #:home [home (find-system-path 'home-dir)]
         #:os [os (system-type 'os)]
         #:env [env getenv]
         #:all-roots [all-roots
                      (with-handlers ([exn:fail? (lambda (_) '())])
                        (filesystem-root-list))])
  (define home-path (simple-form-path home))
  (define base
    (for/list ([name (in-list '("Desktop" "Documents" "Downloads"))]
               #:do [(define path (build-path home-path name))]
               #:when (existing-directory path))
      (simple-form-path path)))
  (define onedrive
    (and (eq? os 'windows)
         (let ([raw (or (env "OneDriveConsumer") (env "OneDrive"))])
           (and (string? raw) (not (string=? raw ""))
                (existing-directory raw)))))
  (define cloud
    (case os
      [(windows)
       (if onedrive
           (for/list ([name (in-list '("Desktop" "Documents"))]
                      #:do [(define path (build-path onedrive name))]
                      #:when (existing-directory path))
             (simple-form-path path))
           '())]
      [(macosx)
       (define icloud
         (build-path home-path "Library" "Mobile Documents"
                     "com~apple~CloudDocs"))
       (for/list ([name (in-list '("Desktop" "Documents"))]
                  #:do [(define path (build-path icloud name))]
                  #:when (existing-directory path))
         (simple-form-path path))]
      [(unix) (xdg-user-roots home-path env)]
      [else '()]))
  (define volumes
    (data-volume-roots #:os os #:all-roots all-roots))
  (remove-duplicates
   (append base cloud volumes)
   (if (eq? os 'windows) string-ci=? string=?)
   #:key path->string))

(define (safe-directory-list directory)
  ;; #f means the directory could not be read; an empty list means it was
  ;; read successfully and is empty. Reconciliation must preserve that
  ;; distinction or a permission denial can look like mass deletion.
  (with-handlers ([exn:fail:filesystem? (lambda (_) #f)]
                  [exn:fail:contract? (lambda (_) #f)])
    (directory-list directory #:build? #t)))

(define (cancelled? cancel)
  (not (false? (sync/timeout 0 (semaphore-peek-evt cancel)))))

(define (path->hit path)
  (with-handlers ([exn:fail? (lambda (_) #f)])
    (define name-path (file-name-from-path path))
    (and name-path
         (cons (path->string name-path)
               (path->string (simple-form-path path))))))

(define (scan-root! index-path root generation cancel status-box
                    indexed completed total batch-size list-directory on-progress)
  (define batch '())
  (define batch-count 0)
  (define count indexed)
  (define complete? #t)
  (define (publish! state [error #f])
    (set-box! status-box
              (file-catalog-status state count completed total error)))
  (define (flush!)
    (when (pair? batch)
      (define hits (reverse batch))
      (unless (file-index-upsert-scan-hits!
               index-path (path->string root) generation hits)
        (error 'file-catalog "index batch write failed"))
      (set! count (+ count batch-count))
      (set! batch '())
      (set! batch-count 0)
      (publish! 'scanning)
      ;; A new searchable batch should refresh the current query, without
      ;; requiring the user to type another character.
      (on-progress)
      ;; Cooperatively yield between database batches so a large catalog does
      ;; not monopolize the embedded backend while the user keeps typing.
      (sleep 0)))
  (let loop ([pending (list root)])
    (cond
      [(cancelled? cancel) (values #f count)]
      [(null? pending)
       (flush!)
       (values (if complete? 'complete 'partial) count)]
      [else
       (define directory (car pending))
       (define rest (cdr pending))
       (define children (list-directory directory))
       (define next
         (if children
             (for/fold ([stack rest]) ([child (in-list children)])
               (cond
                 [(cancelled? cancel) stack]
                 [(and (directory-exists? child)
                       (not (link-exists? child))
                       (not (ignored-directory? child)))
                  (cons child stack)]
                 [(file-exists? child)
                  (define hit (path->hit child))
                  (when hit
                    (set! batch (cons hit batch))
                    (set! batch-count (add1 batch-count))
                    (when (>= batch-count batch-size) (flush!)))
                  stack]
                 [else stack]))
             (begin (set! complete? #f) rest)))
       (loop next)])))

(define (start-file-catalog! index-path roots
                             #:batch-size [batch-size 1000]
                             #:list-directory [list-directory safe-directory-list]
                             #:on-progress [on-progress void]
                             #:on-complete [on-complete void])
  (unless (and (exact-positive-integer? batch-size) (<= batch-size 10000))
    (raise-argument-error 'start-file-catalog! "integer from 1 to 10000" batch-size))
  (define normalized-roots
    (remove-duplicates
     (for/list ([root (in-list roots)]
                #:when (existing-directory root))
       (simple-form-path root))
     string=? #:key path->string))
  (define cancel (make-semaphore 0))
  ;; Throttle search-revision notifications on large directory trees; always
  ;; notify when scanning finishes. Callback failure must not abort indexing.
  (define last-notified #f)
  (define (notify-progress! [force? #f])
    (define now (current-inexact-milliseconds))
    (when (or force? (not last-notified)
              (>= (- now last-notified) 750))
      (set! last-notified now)
      (with-handlers ([exn:fail? (lambda (_) (void))])
        (on-progress))))
  ;; Last generation each root was scanned with. Live reconciles must
  ;; upsert rows with the root's current generation, or the next
  ;; finish-scan would prune freshly reconciled rows that a concurrent
  ;; walk happened to miss.
  (define root-generations (make-hash))
  (define status
    (box (file-catalog-status 'starting 0 0 (length normalized-roots) #f)))
  (define worker-thread
    (thread
     (lambda ()
       (with-handlers
           ([exn:fail?
             (lambda (exn)
               (define previous (unbox status))
               (set-box! status
                         (struct-copy file-catalog-status previous
                                      [state 'failed]
                                      [error (exn-message exn)]))
               (notify-progress! #t))])
         (define indexed 0)
         (define completed 0)
         (define partial? #f)
         (for ([root (in-list normalized-roots)] #:break (cancelled? cancel))
           (define generation
             (+ (* 1000 (inexact->exact
                         (floor (current-inexact-milliseconds))))
                (random 1000)))
           (hash-set! root-generations (path->string root) generation)
           (set-box! status
                     (file-catalog-status 'scanning indexed completed
                                          (length normalized-roots) #f))
           (define-values (outcome next-indexed)
             (scan-root! index-path root generation cancel status
                         indexed completed (length normalized-roots) batch-size
                         list-directory notify-progress!))
           (set! indexed next-indexed)
           (case outcome
             [(complete)
              (unless (file-index-finish-scan!
                       index-path (path->string root) generation)
                (error 'file-catalog "index reconciliation failed"))
              (set! completed (add1 completed))]
             [(partial) (set! partial? #t)]
             [else (void)]))
         (set-box! status
                   (file-catalog-status
                    (cond [(cancelled? cancel) 'cancelled]
                          [partial? 'partial]
                          [else 'ready])
                    indexed completed (length normalized-roots) #f))
         (notify-progress! #t)
         (on-complete)))))
  (file-catalog-worker worker-thread cancel status root-generations
                       index-path))

(define (stop-file-catalog! worker)
  (unless (cancelled? (file-catalog-worker-cancel worker))
    (semaphore-post (file-catalog-worker-cancel worker)))
  (void))

(define (file-catalog-current-status worker)
  (unbox (file-catalog-worker-status worker)))

(define (file-catalog-wait worker [timeout-seconds 30])
  (and (sync/timeout timeout-seconds
                     (thread-dead-evt (file-catalog-worker-thread worker)))
       (file-catalog-current-status worker)))

;; Boundary-aware root containment: a root contains a directory only when
;; the directory IS the root or sits below a separator. A plain
;; string-prefix? let "…\Desktop" claim "…\DesktopBackup" (wrong root,
;; wrong generation), and the drive root "C:\" never matched the stripped
;; notification form "C:" — top-level drive files were always "outside".
(define (catalog-root-contains? root-str dir-str)
  (cond
    [(string=? dir-str root-str) #t]
    [(or (string-suffix? root-str "\\") (string-suffix? root-str "/"))
     (string-prefix? dir-str root-str)]
    [else
     (or (string-prefix? dir-str (string-append root-str "\\"))
         (string-prefix? dir-str (string-append root-str "/")))]))

;; Live-update reconcile (issue #99): after filesystem events touch
;; `directory`, re-list it and reconcile the catalog. The directory must
;; sit under one of the worker's scanned roots — the longest matching
;; root wins, and the root's current scan generation is reused so a
;; concurrent finish-scan cannot prune freshly reconciled rows. Events
;; may be duplicated or lost; this operation is idempotent and lost
;; events are healed by the next full scan. Returns #f when the
;; directory is outside every root or the catalog never scanned it.
(define (reconcile-directory-in-catalog! worker directory)
  ;; The reconcile dir can arrive with a trailing separator — Windows
  ;; notifications derive it from a watcher root that already ends in
  ;; one, and appending sep-string then doubles the separator, which
  ;; never matches the single-separator paths stored in the index. Strip
  ;; every trailing separator before matching; a bare drive letter
  ;; ("C:") is the drive root and keeps its separator so simple-form-path
  ;; cannot resolve it as a drive-relative path.
  (define raw (if (string? directory) directory (path->string directory)))
  (define stripped
    (let strip ([s raw])
      (if (and (> (string-length s) 1)
               (or (string-suffix? s "\\") (string-suffix? s "/")))
          (strip (substring s 0 (sub1 (string-length s))))
          s)))
  (define dir-str
    (if (regexp-match? #rx"^[A-Za-z]:$" stripped)
        (string-append stripped "\\")
        stripped))
  (define dir-path (simple-form-path (string->path dir-str)))
  ;; Root keys are strings (path->string of each scanned root); best
  ;; accumulates the longest root that actually contains this directory.
  (define root-str
    (for/fold ([best #f])
              ([candidate (in-hash-keys
                           (file-catalog-worker-root-generations worker))])
      (if (and (catalog-root-contains? candidate dir-str)
               (or (not best)
                   (> (string-length candidate) (string-length best))))
          candidate
          best)))
  (cond
    [(not root-str) #f]
    ;; The walker never descends ignored trees (node_modules, the system
    ;; drive's Windows/Users roots, …), but a drive-root watcher still
    ;; reports changes inside them; reconciling those would index exactly
    ;; the noise the scan excludes (AppData temp churn through the C:\
    ;; root, dependency trees under a home root). Judge the CUMULATIVE
    ;; path of each component below the matched root with the same
    ;; predicate the walker uses, so OS-name anchoring agrees: the home
    ;; roots sit under "Users" and stay reconcilable through their own,
    ;; longer root key, and a user tree named "windows" below a data root
    ;; reconciles like any other directory.
    [(let loop ([parts (regexp-split
                        #px"[/\\\\]+"
                        (substring dir-str (string-length root-str)))]
                [prefix root-str])
       (cond
         [(null? parts) #f]
         [(string=? (car parts) "") (loop (cdr parts) prefix)]
         [else
          (define child-str
            (if (or (string-suffix? prefix "\\") (string-suffix? prefix "/"))
                (string-append prefix (car parts))
                (string-append prefix
                               (if (string-contains? prefix "\\") "\\" "/")
                               (car parts))))
          (or (ignored-directory? child-str)
              (loop (cdr parts) child-str))]))
     #f]
    [else
     (define listing (safe-directory-list dir-path))
     (and listing
          (let* ([files
                  (for/list ([child (in-list listing)]
                             #:when (and (file-exists? child)
                                         (not (link-exists? child)))
                             #:unless (ignored-directory? child))
                    (path->hit child))]
                 [child-dirs
                  (for/list ([child (in-list listing)]
                             #:when (and (directory-exists? child)
                                         (not (link-exists? child))
                                         (not (ignored-directory? child))))
                    child)]
                 [generation
                  (hash-ref (file-catalog-worker-root-generations worker)
                            root-str 0)])
            (file-index-reconcile-directory!
             (file-catalog-worker-index-path worker)
             root-str generation
             dir-str files child-dirs)))]))
