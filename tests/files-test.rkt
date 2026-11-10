#lang racket/base

;; File search: the mdfind output parser (pure), and the engine provider's
;; two shapes — Spotlight rows on macOS, the honest unavailability row
;; elsewhere.

(require racket/file
         racket/path
         racket/async-channel
         racket/list
         racket/string
         rackunit
         "../app/core/engine.rkt"
         "../app/core/files.rkt"
         "../app/core/search-normalize.rkt")

(define (row-id row) (list-ref row 0))
(define (row-kind row) (list-ref row 3))

(test-case "mdfind output parsing filters junk and dedupes"
  (define parsed
    (parse-paths
     "/Users/me/Reports/q3.md\n/Users/me/Reports/q3.md\nnot a path\n/Users/me/Notes.md\n"))
  (check-equal? (length parsed) 2)
  (check-equal? (car (car parsed)) "q3.md")
  (check-equal? (cdr (car parsed)) "/Users/me/Reports/q3.md")
  ;; Non-string input (a timed-out probe) yields no rows.
  (check-equal? (parse-paths #f) '())
  ;; The limit holds.
  (check-equal? (length (parse-paths "/a/1\n/a/2\n/a/3\n/a/4\n" 3)) 3))

(test-case "Windows Search file: URLs decode to plain paths"
  (define parsed
    (parse-paths
     "file:///C:/Users/me/Reports/q3.md\nfile:///C:/Users/me/My%20Notes.md\n"))
  (check-equal? (length parsed) 2)
  (check-equal? (car (car parsed)) "q3.md")
  (check-equal? (cdr (car parsed)) "C:/Users/me/Reports/q3.md")
  (check-equal? (cdr (cadr parsed)) "C:/Users/me/My Notes.md"))


(test-case "file URL transport keeps arbitrary Windows volumes and UNC authorities"
  ;; Volume labels must never be a fixed list of C:/D:/E:. Indexed
  ;; Windows Search can emit another assigned letter or a network share.
  (check-equal?
   (parse-paths
    "file:///Z:/Work/report.txt\nfile:///Q:/Users/%E6%96%87%E6%A1%A3.txt\nfile://storage.example/Team%20Space/report.txt\n"
    8)
   '(("report.txt" . "Z:/Work/report.txt")
     ("文档.txt" . "Q:/Users/文档.txt")
     ("report.txt" . "//storage.example/Team Space/report.txt"))))

(test-case "file URL transport retains POSIX absolute paths"
  (check-equal?
   (parse-paths "file:///home/runner/notes.txt\nfile:/mnt/archive/log.txt\n")
   '(("notes.txt" . "/home/runner/notes.txt")
     ("log.txt" . "/mnt/archive/log.txt"))))

(test-case "Windows scan paths with backslashes are retained"
  ;; Get-ChildItem -ExpandProperty FullName emits native backslashes.  The old
  ;; slash-only filter silently discarded every fallback result. PowerShell's
  ;; CRLF delimiter must not become part of the path either.
  (define parsed
    (parse-paths
     "C:\\Users\\me\\Reports\\q3.md\r\nD:\\Notes\\My Notes.txt\r\n"))
  (check-equal? parsed
                '(("q3.md" . "C:\\Users\\me\\Reports\\q3.md")
                  ("My Notes.txt" . "D:\\Notes\\My Notes.txt"))))

(test-case "file hits share one deterministic relevance order"
  (define candidates
    '(("old-report.md" . "/home/me/archive/old-report.md")
      ("report-old.md" . "/home/me/report-old.md")
      ("report.md" . "/home/me/report.md")
      ("report.md" . "/home/me/project/node_modules/pkg/report.md")
      ("notes.md" . "/home/me/reports/report-notes.md")))
  (define ranked (rank-file-hits "report" candidates 5))
  ;; Exact stem wins; an equally named generated dependency is demoted. A
  ;; filename prefix beats a match found only in the parent path.
  (check-equal? (cdr (car ranked)) "/home/me/report.md")
  (check-true
   (< (index-of ranked '("report-old.md" . "/home/me/report-old.md"))
      (index-of ranked '("notes.md" . "/home/me/reports/report-notes.md"))))
  (check-true
   (< (index-of ranked '("report.md" . "/home/me/report.md"))
      (index-of ranked
                '("report.md" . "/home/me/project/node_modules/pkg/report.md"))))
  (check-equal? (rank-file-hits "report" candidates 2)
                (take ranked 2)))

(test-case "cross-platform reranking recognizes pinyin candidates"
  (define ranked
    (rank-file-hits
     "weixin"
     '(("unrelated.txt" . "C:\\Users\\me\\unrelated.txt")
       ("微信计划.md" . "C:\\Users\\me\\Documents\\微信计划.md"))
     2))
  (check-equal? (car (car ranked)) "微信计划.md"))

(test-case "Windows explicit find checks known folders before Windows Search"
  (define captured #f)
  (define captured-timeout-ms #f)
  (define result
    (file-search/status
     "o'brien" 8
     #:os 'windows
     #:resolve (lambda (program) (and (string=? program "powershell.exe") program))
     #:run (lambda (program args #:timeout-ms [timeout-ms 3000]
                               #:cancel-evt [_cancel-evt never-evt])
             (set! captured (cons program args))
             (set! captured-timeout-ms timeout-ms)
             "file:///C:/Users/me/obrien.txt\n")))
  ;; Only explicit Windows find receives a longer bounded deadline.
  (check-equal? captured-timeout-ms 9000)
  (check-equal? (car captured) "powershell.exe")
  (check-equal? (cadr captured) "-NoProfile")
  (check-equal? (file-search-result-status result) 'ok)
  (check-equal? (file-search-result-hits result)
                '(("obrien.txt" . "C:/Users/me/obrien.txt")))
  (define script (cadddr captured))
  ;; Local known folders precede the optional, sometimes-hung OLE DB index.
  (check-true (string-contains? script "GetFolderPath('MyDocuments')"))
  (check-true (string-contains? script "OneDriveConsumer"))
  (check-true (string-contains? script "if($out.Count -eq 0){try{"))
  (check-true (string-contains? script "SYSTEMINDEX"))
  (check-true (string-contains? script "CONTAINS(System.FileName,"))
  (check-true (string-contains? script "FREETEXT(System.FileName,"))
  (check-true (string-contains? script "Get-ChildItem"))
  (check-true (string-contains? script "SELECT TOP 128"))
  (check-true (string-contains? script "Select-Object -First 128"))
  (check-true (string-contains? script "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)")
              "Windows PowerShell must emit UTF-8 path names to the Racket UTF-8 reader")
  (check-false (string-contains? script "-replace '^file:/+'")
               "indexed file URLs must preserve UNC //server/share authorities")
  ;; The query is neutralized for both SQL and PowerShell single quotes.
  (check-false (string-contains? script "o'brien"))
  (check-true (string-contains? script "o''brien")))

(test-case "Windows find returns a real Downloads file using the production PowerShell"
  (when (eq? (system-type 'os) 'windows)
    (define profile (getenv "USERPROFILE"))
    (when profile
      (define downloads (build-path profile "Downloads"))
      (define existed? (directory-exists? downloads))
      (define fixture-name (format "fulcrum-find-~a.txt"
                                   (current-process-milliseconds)))
      (define fixture (build-path downloads fixture-name))
      (define query (substring fixture-name 0 (- (string-length fixture-name) 4)))
      (make-directory* downloads)
      (dynamic-wind
        (lambda () (display-to-file "fixture" fixture #:exists 'truncate))
        (lambda ()
          (define found (file-search/status query 8 #:os 'windows))
          (check-equal? (file-search-result-status found) 'ok)
          (check-true (for/or ([hit (in-list (file-search-result-hits found))])
                        (string-ci=? (car hit) fixture-name))))
        (lambda ()
          (when (file-exists? fixture) (delete-file fixture))
          (unless existed?
            (with-handlers ([exn:fail? (lambda (_) (void))])
              (delete-directory downloads))))))))

(test-case "failed local index reports an error instead of false no matches"
  (define dir (make-temporary-file "fulcrum-broken-search-~a" 'directory))
  (dynamic-wind
    void
    (lambda ()
      (define blocker (build-path dir "not-a-directory"))
      (define db (build-path blocker "file-index.sqlite3"))
      (display-to-file "blocker" blocker)
      (let ([absent
             (file-search/status
              "report" 8 #:os 'unix #:resolve (lambda (_) #f)
              #:index-path db)])
        (check-equal? (file-search-result-status absent) 'index-error)
        (check-equal? (file-search-result-hits absent) '()))
      ;; A usable platform index can still provide valid results even if
      ;; Fulcrum's optional local cache is temporarily inaccessible.
      (let ([platform
             (file-search/status
              "report" 8 #:os 'unix #:resolve (lambda (_) "/usr/bin/locate")
              #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                           #:cancel-evt [_cancel-evt never-evt])
                      "/home/me/report.md\n")
              #:index-path db)])
        (check-equal? (file-search-result-status platform) 'ok)
        (check-equal? (file-search-result-hits platform)
                      '(("report.md" . "/home/me/report.md")))))
    (lambda () (delete-directory/files dir))))

(test-case "Linux find fallback reports timeout instead of an empty success"
  (define (find-only program)
    (and (string=? program "find") "/usr/bin/find"))
  (define timed-out
    (file-search/status
     "report" 8
     #:os 'unix
     #:resolve find-only
     #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                   #:cancel-evt [_cancel-evt never-evt]) #f)))
  (check-equal? (file-search-result-status timed-out) 'timeout)
  (check-equal? (file-search-result-backend timed-out) 'find)
  (check-equal? (file-search-result-hits timed-out) '())
  (define completed
    (file-search/status
     "report" 8
     #:os 'unix
     #:resolve find-only
     #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                   #:cancel-evt [_cancel-evt never-evt])
             "/home/me/report.txt\n")))
  (check-equal? (file-search-result-status completed) 'ok)
  (check-equal? (file-search-result-hits completed)
                '(("report.txt" . "/home/me/report.txt"))))

(test-case "a newer claim of the same query cancels the superseded backend"
  ;; Cancellation is a resource guard for re-runs of the same text (the
  ;; revision nudge), not a correctness tool — the host generation-guards
  ;; stale responses on its own.
  (define first-started (make-semaphore 0))
  (define calls 0)
  (define (runner _program _args #:timeout-ms [_timeout-ms 3000]
                                  #:cancel-evt [cancel-evt never-evt])
    (set! calls (add1 calls))
    (if (= calls 1)
        (begin
          (semaphore-post first-started)
          (sync cancel-evt)
          'cancelled)
        "/home/me/new-report.txt\n"))
  (define first-result (make-async-channel 1))
  (define first-worker
    (thread
     (lambda ()
       (async-channel-put
        first-result
        (file-search/status "report" 8
                            #:os 'unix
                            #:resolve (lambda (_) "/usr/bin/find")
                            #:run runner)))))
  (sync first-started)
  (define newest
    (file-search/status "report" 8
                        #:os 'unix
                        #:resolve (lambda (_) "/usr/bin/find")
                        #:run runner))
  (define superseded (sync/timeout 1 first-result))
  (check-equal? (file-search-result-status superseded) 'cancelled)
  (check-equal? (file-search-result-status newest) 'ok)
  (check-equal? (file-search-result-hits newest)
                '(("new-report.txt" . "/home/me/new-report.txt")))
  (check-not-false (sync/timeout 1 (thread-dead-evt first-worker))))

(test-case "a different query never cancels another query's backend search"
  ;; RVT1 workers run concurrently, so claim order is not request order.
  ;; Cancelling across queries let an older request kill the newest one's
  ;; platform stage — on Windows the newest visible query then painted
  ;; with its file rows silently missing (issue: Windows find broken).
  (define first-started (make-semaphore 0))
  (define calls 0)
  (define first-was-cancelled? #f)
  (define (runner _program _args #:timeout-ms [_timeout-ms 3000]
                                  #:cancel-evt [cancel-evt never-evt])
    (set! calls (add1 calls))
    (cond
      [(= calls 1)
       (semaphore-post first-started)
       ;; The "report" claim lands while this worker runs; it must leave
       ;; this "old" search alone.
       (when (sync/timeout 2 cancel-evt)
         (set! first-was-cancelled? #t))
       "/home/me/old-report.txt\n"]
      [else "/home/me/new-report.txt\n"]))
  (define first-result (make-async-channel 1))
  (define first-worker
    (thread
     (lambda ()
       (async-channel-put
        first-result
        (file-search/status "old" 8
                            #:os 'unix
                            #:resolve (lambda (_) "/usr/bin/find")
                            #:run runner)))))
  (sync first-started)
  (define newest
    (file-search/status "report" 8
                        #:os 'unix
                        #:resolve (lambda (_) "/usr/bin/find")
                        #:run runner))
  (define first-outcome (sync/timeout 3 first-result))
  (check-false first-was-cancelled?
               "an older request claiming late must not cancel the newest query")
  (check-equal? (file-search-result-status first-outcome) 'ok)
  (check-equal? (file-search-result-hits first-outcome)
                '(("old-report.txt" . "/home/me/old-report.txt")))
  (check-equal? (file-search-result-status newest) 'ok)
  (check-not-false (sync/timeout 1 (thread-dead-evt first-worker))))

(test-case "a cancelled platform stage still answers from the local index"
  ;; The newest visible query can legitimately be cancelled (a same-query
  ;; re-claim from the revision nudge). Dropping the already-gathered
  ;; catalog hits then left Windows users with a results list stuck on
  ;; unrelated app rows; cancellation must degrade to the cache exactly
  ;; like a timeout does.
  (define dir (make-temporary-file "fulcrum-cancel-cache-~a" 'directory))
  (define index-path (build-path dir "index.sqlite3"))
  (dynamic-wind
    void
    (lambda ()
      (define seeded
        (file-search/status
         "report" 8
         #:os 'unix
         #:resolve (lambda (_) "/usr/bin/locate")
         #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                       #:cancel-evt [_cancel-evt never-evt])
                 "/home/me/report.md\n")
         #:index-path index-path))
      (check-equal? (file-search-result-status seeded) 'ok)
      (define started (make-semaphore 0))
      (define cancelled-runner
        (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                              #:cancel-evt [cancel-evt never-evt])
          (semaphore-post started)
          (sync cancel-evt)
          'cancelled))
      (define blocked-result (make-async-channel 1))
      (define blocked-worker
        (thread
         (lambda ()
           (async-channel-put
            blocked-result
            (file-search/status
             "report" 8
             #:os 'unix
             #:resolve (lambda (_) "/usr/bin/locate")
             #:run cancelled-runner
             #:index-path index-path)))))
      (sync started)
      ;; A same-query re-claim (the revision nudge) cancels the platform
      ;; stage; the catalog hits it already collected must survive.
      (file-search/status
       "report" 8
       #:os 'unix
       #:resolve (lambda (_) "/usr/bin/locate")
       #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                     #:cancel-evt [_cancel-evt never-evt])
               "/home/me/fresh-report.md\n")
       #:index-path index-path)
      (define rescued (sync/timeout 3 blocked-result))
      (check-equal? (file-search-result-status rescued) 'ok)
      (check-equal? (file-search-result-backend rescued) 'fulcrum-index)
      (check-equal? (file-search-result-hits rescued)
                    '(("report.md" . "/home/me/report.md")))
      (check-not-false (sync/timeout 1 (thread-dead-evt blocked-worker))))
    (lambda () (delete-directory/files dir))))

(test-case "a ready live catalog answers find queries without spawning the platform tool"
  ;; Once the catalog is fully scanned (the watcher keeps it live) it is
  ;; the authoritative source on Windows: spawning powershell.exe per
  ;; query added seconds of latency for coverage the catalog already
  ;; holds — the Everything-level instant bar. A thin or still-scanning
  ;; catalog keeps the platform stage.
  (define dir (make-temporary-file "fulcrum-fast-path-~a" 'directory))
  (define index-path (build-path dir "index.sqlite3"))
  (dynamic-wind
    void
    (lambda ()
      ;; Seed the catalog.
      (file-search/status
       "report" 8
       #:os 'unix
       #:resolve (lambda (_) "/usr/bin/locate")
       #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                     #:cancel-evt [_cancel-evt never-evt])
               "/home/me/report.md\n")
       #:index-path index-path)
      (define platform-ran? #f)
      (define instant
        (file-search/status
         "report" 8
         #:os 'windows
         #:resolve (lambda (program) (and (string=? program "powershell.exe") program))
         #:run (lambda (_program _args #:timeout-ms [_timeout-ms 9000]
                                       #:cancel-evt [_cancel-evt never-evt])
                 (set! platform-ran? #t)
                 "file:///C:/Users/me/platform-report.txt\n")
         #:index-path index-path
         #:catalog-ready? #t))
      (check-false platform-ran? "the platform tool must not spawn on a ready catalog")
      (check-equal? (file-search-result-status instant) 'ok)
      (check-equal? (file-search-result-backend instant) 'fulcrum-index)
      (check-equal? (file-search-result-hits instant)
                    '(("report.md" . "/home/me/report.md")))
      ;; A still-scanning catalog keeps the platform stage: the local
      ;; answers are partial and the OS index fills the gaps.
      (define scanning
        (file-search/status
         "report" 8
         #:os 'windows
         #:resolve (lambda (program) (and (string=? program "powershell.exe") program))
         #:run (lambda (_program _args #:timeout-ms [_timeout-ms 9000]
                                       #:cancel-evt [_cancel-evt never-evt])
                 (set! platform-ran? #t)
                 "file:///C:/Users/me/platform-report.txt\n")
         #:index-path index-path
         #:catalog-ready? #f))
      (check-true platform-ran? "a scanning catalog keeps the platform stage")
      (check-equal? (file-search-result-status scanning) 'ok))
    (lambda () (delete-directory/files dir))))

(test-case "platform results seed the persistent index and rescue a timeout"
  (define dir (make-temporary-file "fulcrum-search-index-~a" 'directory))
  (define index-path (build-path dir "index.sqlite3"))
  (dynamic-wind
    void
    (lambda ()
      (define seeded
        (file-search/status
         "report" 8
         #:os 'unix
         #:resolve (lambda (_) "/usr/bin/locate")
         #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                       #:cancel-evt [_cancel-evt never-evt])
                 "/home/me/report.md\n")
         #:index-path index-path))
      (check-equal? (file-search-result-status seeded) 'ok)
      (define rescued
        (file-search/status
         "report" 8
         #:os 'unix
         #:resolve (lambda (_) "/usr/bin/locate")
         #:run (lambda (_program _args #:timeout-ms [_timeout-ms 3000]
                                       #:cancel-evt [_cancel-evt never-evt]) #f)
         #:index-path index-path))
      (check-equal? (file-search-result-status rescued) 'ok)
      (check-equal? (file-search-result-backend rescued) 'fulcrum-index)
      (check-equal? (file-search-result-hits rescued)
                    '(("report.md" . "/home/me/report.md")))
      ;; The same cache is useful even when the host search utility disappears.
      (define offline
        (file-search/status "report" 8
                            #:os 'unix
                            #:resolve (lambda (_) #f)
                            #:index-path index-path))
      (check-equal? (file-search-result-status offline) 'ok)
      (check-equal? (file-search-result-hits offline)
                    '(("report.md" . "/home/me/report.md"))))
    (lambda () (delete-directory/files dir))))

(test-case "an explicit find query leads with file rows, fuzzy matches follow"
  ;; App fuzzy matches score 100+ and otherwise bury the file surface
  ;; below the fold: a `find` query names the file surface outright, so
  ;; its rows lead regardless of score (#193 follow-up; the #116
  ;; acceptance criterion is that the probe hit is the first row).
  (define engine (make-engine #:clipboard-store #f
                              #:snippet-store #f
                              #:plugin-manager #f))
  ;; Inject one fuzzy-matching app ('f' first char passes the provider's
  ;; prefilter; "settings" scores against the query) so the ranking
  ;; competition is deterministic on every OS.
  (set-engine-apps-prepared!
   engine
   (list (vector "a-finder-settings"
                 "Finder Settings"
                 "C:/fixtures/FinderSettings.lnk"
                 ""
                 ""
                 (search-fields-with-pinyin
                  (list (cons 1.0 "Finder Settings")
                        (cons 0.7 "")
                        (cons 0.4 "C:/fixtures/FinderSettings.lnk"))))))
  (define rows (engine-search engine "find settings"))
  (define kinds (map (lambda (r) (list-ref r 3)) rows))
  (define first-file (index-of kinds "File"))
  (define first-app (index-of kinds "Application"))
  (check-not-false first-file "a find query must surface at least one file-area row")
  (when (and first-file first-app)
    (check-true (< first-file first-app)
                "file rows must lead fuzzy app matches for explicit find queries")))

(test-case "engine provider: honest row without Spotlight, real rows with it"
  (define engine (make-engine #:clipboard-store #f
                              #:snippet-store #f
                              #:plugin-manager #f))
  ;; The provider only claims "find …" queries: an unrelated query may
  ;; hit other providers (web fallback), but never a file row.
  (define unrelated (engine-search engine "unrelated"))
  (check-false (findf (lambda (r) (equal? (row-id r) "file.open")) unrelated))
  (define rows (engine-search engine "find anything"))
  (define file-rows
    (filter (lambda (r) (or (equal? (row-id r) "file.open")
                            (equal? (row-id r) "noop")))
            rows))
  (if (file-search-available?)
      ;; A search tool on PATH does not guarantee it finishes within 3s.
      ;; Especially on Windows, a cold unindexed PowerShell scan can time out
      ;; on a shared CI runner. In that case a visible timeout explanation is
      ;; the correct product behavior; never mistake it for missing tools.
      (check-true
       (or (and (pair? file-rows)
                (andmap (lambda (r) (equal? (row-id r) "file.open")) file-rows))
           (and (= (length file-rows) 1)
                (equal? (row-id (car file-rows)) "noop")
                (let ([title (list-ref (car file-rows) 1)])
                  (or (string-contains? title "timed out")
                      ;; Empty system results no longer silently return
                      ;; zero rows. The index/scope limitation is explicit.
                      (string-contains? title "No matching files"))))))
      ;; Everywhere else: exactly one honest explanation, which runs to a
      ;; quiet refusal.
      (begin
        (check-equal? (length file-rows) 1)
        (check-equal? (row-id (car file-rows)) "noop")
        (check-true (string-contains?
                     (car (engine-run engine "noop" ""))
                     "not available")))))

(test-case "fixed-volume discovery parses drive letters and fails soft"
  ;; The catalog indexes Windows data volumes full-depth (issue #116), so
  ;; the drive-type filter decides both coverage and safety: Fixed only —
  ;; mapped shares and removable media stay out of every walk.
  (define (fake-runner output)
    (lambda (program args #:timeout-ms t #:cancel-evt c) output))
  (define fixed-output
    "C\nD\nE\n")
  (define mixed-output
    "C\nX\n")  ;; X would be a network/removable letter a second filter dropped
  (define (letters roots)
    (map (lambda (r) (substring (path->string r) 0 1)) roots))
  ;; Non-Windows platforms never enumerate volumes.
  (check-equal? (windows-fixed-volume-roots #:os 'macosx
                                            #:run (fake-runner fixed-output))
                '())
  (check-equal? (windows-fixed-volume-roots #:os 'unix
                                            #:run (fake-runner fixed-output))
                '())
  ;; Windows + PowerShell output → one root per fixed drive, sorted order.
  (check-equal? (letters (windows-fixed-volume-roots
                          #:os 'windows #:run (fake-runner fixed-output)))
                '("C" "D" "E"))
  (check-equal? (letters (windows-fixed-volume-roots
                          #:os 'windows #:run (fake-runner mixed-output)))
                '("C" "X"))
  ;; A PowerShell timeout/failure (runner returns #f) yields no volume
  ;; roots: the catalog falls back to home coverage, honestly.
  (check-equal? (windows-fixed-volume-roots
                 #:os 'windows
                 #:run (lambda (program args #:timeout-ms t #:cancel-evt c) #f))
                '())
  ;; 'cancelled is equally a soft failure.
  (check-equal? (windows-fixed-volume-roots
                 #:os 'windows
                 #:run (lambda (program args #:timeout-ms t #:cancel-evt c)
                         'cancelled))
                '()))

(test-case "file rows reveal the containing folder as a secondary action"
  ;; Enter opens the file; the ⌘K panel answers "where is this file
  ;; even" with the containing folder, plus copy-path.
  (define actions (engine-row-actions (make-engine #:clipboard-store #f
                                                   #:snippet-store #f
                                                   #:plugin-manager #f)
                                      "file.open"
                                      "/home/me/reports/fota_udp.xlsx"))
  (check-equal? (length actions) 2)
  (check-equal? (list-ref (car actions) 0) "file.reveal")
  (check-equal? (list-ref (car actions) 1) "Open containing folder")
  (check-equal? (list-ref (car actions) 4)
                "/home/me/reports/fota_udp.xlsx")
  (check-equal? (list-ref (cadr actions) 0) "copy"))
