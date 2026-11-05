#lang racket/base

(require racket/file
         racket/list
         racket/path
         racket/string
         rackunit
         "../app/core/file-catalog.rkt"
         "../app/core/file-index.rkt"
         "../app/core/engine.rkt")

(define (with-temp-tree thunk)
  (define dir (make-temporary-file "fulcrum-catalog-~a" 'directory))
  (dynamic-wind void (lambda () (thunk dir))
                (lambda () (delete-directory/files dir))))

(define (touch path [text "fixture"])
  (make-parent-directory* path)
  (display-to-file text path #:exists 'truncate))

(test-case "default roots are bounded to common folders and OneDrive"
  (with-temp-tree
   (lambda (home)
     (for ([name (in-list '("Desktop" "Documents" "Downloads"))])
       (make-directory (build-path home name)))
     (define cloud (build-path home "OneDrive"))
     (make-directory cloud)
     (make-directory (build-path cloud "Documents"))
     (define roots
       (default-file-catalog-roots
        #:home home
        #:os 'windows
        #:env (lambda (name)
                (and (string=? name "OneDriveConsumer")
                     (path->string cloud)))
        ;; Hermetic on Windows runners: no volume enumeration, this test
        ;; is about the home/OneDrive roots only.
        #:all-roots '()))
     (check-equal? (length roots) 4)
     (check-false (member (simple-form-path home) roots))
     (check-not-false (member (simple-form-path (build-path home "Documents"))
                              roots)))))

(test-case "Linux roots honor localized XDG user directories"
  (with-temp-tree
   (lambda (home)
     (define localized (build-path home "文档"))
     (make-directory localized)
     (define config-dir (build-path home ".config"))
     (make-directory config-dir)
     (display-to-file
      "XDG_DOCUMENTS_DIR=\"$HOME/文档\"\nXDG_DOWNLOAD_DIR=\"$HOME/不存在\"\n"
      (build-path config-dir "user-dirs.dirs"))
     (define roots
       (default-file-catalog-roots #:home home #:os 'unix
                                   #:env (lambda (_) #f)))
     (check-equal? roots (list (simple-form-path localized))))))

(test-case "catalog scans incrementally, ignores dependency trees, and reconciles"
  (with-temp-tree
   (lambda (dir)
     (define root (build-path dir "Documents"))
     (define db (build-path dir "index.sqlite3"))
     (define alpha (build-path root "alpha-report.txt"))
     (define beta (build-path root "team" "beta-notes.md"))
     (define ignored (build-path root "project" "node_modules" "secret.js"))
     (touch alpha)
     (touch beta)
     (touch ignored)
     (define notifications (box '()))
     (define first
       (start-file-catalog!
        db (list root) #:batch-size 1
        #:on-progress
        (lambda ()
          ;; Notifications fire only after the new paths are committed.
          (set-box! notifications
                    (cons (file-index-count db) (unbox notifications))))))
     (define first-status (file-catalog-wait first 10))
     (check-not-false first-status)
     (check-equal? (file-catalog-status-state first-status) 'ready)
     (check-equal? (file-catalog-status-indexed first-status) 2)
     (check-true (>= (length (unbox notifications)) 2)
                 "one first-batch update and a final refresh")
     (check-equal? (last (unbox notifications)) 1
                   "the first batch must be visible before its nudge")
     (check-equal? (car (unbox notifications)) 2
                   "the finished index must be visible at the final nudge")
     (check-equal? (file-index-count db) 2)
     (check-equal? (map car (file-index-search db "alpha" 8))
                   '("alpha-report.txt"))
     (check-equal? (file-index-search db "secret" 8) '())

     (delete-file alpha)
     (define renamed (build-path root "team" "gamma-notes.md"))
     (rename-file-or-directory beta renamed)
     (define second (start-file-catalog! db (list root) #:batch-size 2))
     (define second-status (file-catalog-wait second 10))
     (check-not-false second-status)
     (check-equal? (file-catalog-status-state second-status) 'ready)
     (check-equal? (file-index-count db) 1)
     (check-equal? (file-index-search db "alpha" 8) '())
     (check-equal? (file-index-search db "beta" 8) '())
     (check-equal? (map car (file-index-search db "gamma" 8))
                   '("gamma-notes.md")))))

(test-case "catalog with no existing roots completes without touching user folders"
  (with-temp-tree
   (lambda (dir)
     (define worker
       (start-file-catalog! (build-path dir "index.sqlite3")
                            (list (build-path dir "missing"))))
     (define status (file-catalog-wait worker 5))
     (check-not-false status)
     (check-equal? (file-catalog-status-state status) 'ready)
     (check-equal? (file-catalog-status-roots-total status) 0)
     (check-equal? (file-catalog-status-indexed status) 0))))

(test-case "partial scan preserves old rows instead of treating denial as deletion"
  (with-temp-tree
   (lambda (dir)
     (define root (build-path dir "Documents"))
     (define blocked (build-path root "blocked"))
     (define old (build-path blocked "old.txt"))
     (define db (build-path dir "index.sqlite3"))
     (touch old)
     (check-equal? (file-catalog-status-state
                    (file-catalog-wait (start-file-catalog! db (list root)) 10))
                   'ready)
     (delete-file old)
     (define partial
       (start-file-catalog!
        db (list root)
        #:list-directory
        (lambda (path)
          (if (equal? (simple-form-path path) (simple-form-path blocked))
              #f
              (directory-list path #:build? #t)))))
     (define status (file-catalog-wait partial 10))
     (check-equal? (file-catalog-status-state status) 'partial)
     ;; The stale row is safer than destructive reconciliation after an
     ;; unreadable subtree; the next complete scan will remove it.
     (check-equal? (map car (file-index-search db "old" 8)) '("old.txt")))))

(test-case "engine owns one lazy catalog worker and exposes its status"
  (with-temp-tree
   (lambda (dir)
     (define root (build-path dir "Desktop"))
     (define db (build-path dir "index.sqlite3"))
     (touch (build-path root "launcher-plan.md"))
     (define refresh-count (box 0))
     (define engine
       (make-engine #:clipboard-store #f
                    #:snippet-store #f
                    #:plugin-manager #f
                    #:file-index-path db
                    #:file-catalog-roots (list root)
                    #:file-catalog-nudge
                    (lambda ()
                      (set-box! refresh-count (add1 (unbox refresh-count))))))
     (check-equal? (file-catalog-status-state
                    (engine-file-catalog-status engine))
                   'idle)
     (define idle-row
       (findf (lambda (row) (string-contains? (cadr row) "ready to start"))
              (engine-search engine "file index")))
     (check-not-false idle-row)
     (engine-start-file-catalog! engine)
     ;; A duplicate start returns the same owned worker rather than racing a
     ;; second full scan against it.
     (engine-start-file-catalog! engine)
     (let wait ([remaining 100])
       (define status (engine-file-catalog-status engine))
       (cond
         [(eq? (file-catalog-status-state status) 'ready)
          (check-true (positive? (unbox refresh-count))
                      "native frontends need a query refresh after indexing")
         (check-equal? (map car (file-index-search db "launcher" 8))
                        '("launcher-plan.md"))
          (check-not-false
           (findf (lambda (row) (string-contains? (cadr row) "File index ready"))
                  (engine-search engine "file index")))]
         [(zero? remaining)
          (fail-check "engine catalog did not finish")]
         [else (sleep 0.02) (wait (sub1 remaining))]))
     (engine-stop! engine))))

(test-case "every lettered Windows drive becomes a catalog root"
  ;; Multi-drive machines were invisible to `find`: macOS Spotlight and
  ;; Linux plocate cover whole disks, but on Windows only the user folders
  ;; (and whatever the OS index reaches) were searchable (issue #116).
  ;; The system drive is included too — excluding it left user-created
  ;; directories like C:\FulcrumTest unfindable; the walker's ignore list
  ;; keeps the OS trees (Windows, Program Files, Users) out of the walk.
  (with-temp-tree
   (lambda (home)
     (for ([name (in-list '("Desktop" "Documents" "Downloads"))])
       (make-directory (build-path home name)))
     (define roots
       (default-file-catalog-roots
        #:home home
        #:os 'windows
        #:env (lambda (_) #f)
        #:all-roots '("C:\\" "D:\\" "E:\\")))
     ;; Drive-root check must be exact: on a runner whose temp home sits
     ;; under C:, the home roots share the C: prefix — only the bare
     ;; volume root itself counts as "the C: drive is a catalog root".
     (define (has-drive-root? letter)
       (and (memf (lambda (r)
                    (string=? (path->string r)
                              (string-append letter ":\\")))
                  roots)
            #t))
     (check-true (has-drive-root? "C"))
     (check-true (has-drive-root? "D"))
     (check-true (has-drive-root? "E"))
     ;; The three home user roots plus three drive roots.
     (check-equal? (length roots) 6))))

(test-case "a single-drive Windows machine gains no volume roots"
  (with-temp-tree
   (lambda (home)
     (for ([name (in-list '("Desktop" "Documents" "Downloads"))])
       (make-directory (build-path home name)))
     (define roots
       (default-file-catalog-roots
        #:home home
        #:os 'windows
        #:env (lambda (_) #f)
        #:all-roots '("C:\\")))
     ;; Desktop/Documents/Downloads (the temp home provides all three;
     ;; OneDrive env is off) plus the C: drive root itself.
     (check-equal? (length roots) 4))))

(test-case "volume classification claims only drive-letter roots"
  (with-temp-tree
   (lambda (home)
     (define roots
       (default-file-catalog-roots
        #:home home
        #:os 'windows
        #:env (lambda (_) #f)
        #:all-roots '("C:\\" "D:\\" "\\\\server\\share" "not-a-drive")))
     ;; UNC shares and junk strings are not drive roots; every classified
     ;; root names a lettered drive.
     (check-true
      (andmap (lambda (r)
                (regexp-match? #rx"^[CD]:" (path->string r)))
              roots))
     (check-equal? (length roots) 2))))

(test-case "volume system metadata directories are ignored by the walker"
  (with-temp-tree
   (lambda (dir)
     (define root (build-path dir "data"))
     (make-directory* (build-path root "$RECYCLE.BIN" "SID"))
     (make-directory* (build-path root "System Volume Information"))
     (make-directory* (build-path root "Windows" "System32"))
     (touch (build-path root "Windows" "os-file.txt"))
     (make-directory* (build-path root "Program Files" "App"))
     (touch (build-path root "keep-me.txt"))
     (make-directory* (build-path root "FulcrumTest"))
     (touch (build-path root "FulcrumTest" "partition-probe.txt"))
     (define db (build-path dir "index.sqlite3"))
     (define worker (start-file-catalog! db (list root) #:on-progress void))
     (define status (file-catalog-wait worker 10))
     (check-equal? (file-catalog-status-state status) 'ready)
     (check-equal? (map car (file-index-search db "keep" 8))
                   '("keep-me.txt"))
     (check-equal? (file-index-search db "junk" 8) '())
     ;; OS trees are never descended, but user-created trees on the same
     ;; drive root are indexed full-depth (the C:\FulcrumTest probe from
     ;; issue #116's acceptance).
     (check-equal? (file-index-search db "os-file" 8) '())
     (check-equal? (map car (file-index-search db "partition-probe" 8))
                   '("partition-probe.txt"))
     (stop-file-catalog! worker))))
