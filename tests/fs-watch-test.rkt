#lang racket/base

;; Issue #99 slice-1 tests: directory-level reconcile semantics (every
;; platform — these exercise the pure SQLite + listing logic through the
;; public catalog worker API), the fs-watch dispatcher's platform
;; contract, and — on Windows CI — the real ReadDirectoryChangesW
;; watcher end-to-end: a created file must become searchable without
;; retyping, and a deleted one must leave the results, both well inside
;; the two-second mutation-visibility SLO's test budget.

(require racket/file
         racket/list
         racket/path
         racket/string
         rackunit
         "../app/core/file-catalog.rkt"
         "../app/core/file-index.rkt"
         "../app/core/engine.rkt"
         "../app/core/fs-watch.rkt")

(define (with-temp-tree thunk)
  (define dir (make-temporary-file "fulcrum-fswatch-~a" 'directory))
  (dynamic-wind void (lambda () (thunk dir))
                (lambda () (delete-directory/files dir))))

(define (touch path [text "fixture"])
  (make-parent-directory* path)
  (display-to-file text path #:exists 'truncate))

(test-case "reconcile makes a created file searchable and a deleted one absent"
  (with-temp-tree
   (lambda (root)
     (make-directory (build-path root "Documents"))
     (define db (build-path root "index.sqlite3"))
     (define worker
       (start-file-catalog! db (list (build-path root "Documents"))
                            #:on-progress void))
     (check-equal? (file-catalog-status-state (file-catalog-wait worker 10))
                   'ready)
     ;; Baseline: one indexed file.
     (touch (build-path root "Documents" "alpha.txt"))
     (check-true (reconcile-directory-in-catalog!
                  worker (build-path root "Documents")))
     (check-equal? (map car (file-index-search db "alpha" 8))
                   '("alpha.txt"))
     ;; Create → reconcile → visible.
     (touch (build-path root "Documents" "beta.txt"))
     (reconcile-directory-in-catalog! worker (build-path root "Documents"))
     (check-equal? (file-index-search db "beta" 8)
                   (list (cons "beta.txt"
                               (path->string
                                (simple-form-path
                                 (build-path root "Documents" "beta.txt"))))))
     ;; Delete → reconcile → absent (exact row pruned).
     (delete-file (build-path root "Documents" "beta.txt"))
     (reconcile-directory-in-catalog! worker (build-path root "Documents"))
     (check-equal? (file-index-search db "beta" 8) '())
     (check-equal? (map car (file-index-search db "alpha" 8))
                   '("alpha.txt"))
     (stop-file-catalog! worker))))

(test-case "reconcile prunes the indexed subtree of a vanished directory"
  (with-temp-tree
   (lambda (root)
     (make-directory (build-path root "data"))
     (define db (build-path root "index.sqlite3"))
     (define worker
       (start-file-catalog! db (list (build-path root "data"))
                            #:on-progress void))
     (file-catalog-wait worker 10)
     (make-directory* (build-path root "data" "sub" "deep"))
     (touch (build-path root "data" "sub" "deep" "gone.txt"))
     (touch (build-path root "data" "sub" "kept-sibling.txt"))
     ;; Per-directory semantics: each changed directory is reconciled by
     ;; its own event — the watcher's coalescer maps paths to parents, so
     ;; a real create in deep/ reconciles deep/, then sub/ (its parent,
     ;; for the child-directory bookkeeping), exactly as the live flow
     ;; would replay them.
     (reconcile-directory-in-catalog! worker (build-path root "data" "sub" "deep"))
     (reconcile-directory-in-catalog! worker (build-path root "data" "sub"))
     (check-equal? (file-index-search db "gone" 8)
                   (list (cons "gone.txt"
                               (path->string
                                (simple-form-path
                                 (build-path root "data" "sub" "deep"
                                             "gone.txt"))))))
     ;; Vanish the whole sub directory: reconciling the surviving parent
     ;; must prune the vanished child's exact row and its whole indexed
     ;; subtree.
     (delete-directory/files (build-path root "data" "sub"))
     (reconcile-directory-in-catalog! worker (build-path root "data"))
     (check-equal? (file-index-search db "gone" 8) '())
     (check-equal? (file-index-search db "kept-sibling" 8) '())
     (stop-file-catalog! worker))))

(test-case "reconcile is idempotent under duplicated events"
  (with-temp-tree
   (lambda (root)
     (make-directory (build-path root "docs"))
     (define db (build-path root "index.sqlite3"))
     (define worker
       (start-file-catalog! db (list (build-path root "docs"))
                            #:on-progress void))
     (file-catalog-wait worker 10)
     (touch (build-path root "docs" "report.txt"))
     (define dir (build-path root "docs"))
     ;; Duplicated delivery of the same event must not double-count.
     (reconcile-directory-in-catalog! worker dir)
     (define count-after-first (file-index-count db))
     (reconcile-directory-in-catalog! worker dir)
     (reconcile-directory-in-catalog! worker dir)
     (check-equal? (file-index-count db) count-after-first)
     (check-equal? (file-index-search db "report" 8)
                   (list (cons "report.txt"
                               (path->string
                                (simple-form-path
                                 (build-path root "docs" "report.txt"))))))
     (stop-file-catalog! worker))))

(test-case "directories outside every scanned root are declined"
  (with-temp-tree
   (lambda (root)
     (make-directory (build-path root "docs"))
     (make-directory (build-path root "elsewhere"))
     (define db (build-path root "index.sqlite3"))
     (define worker
       (start-file-catalog! db (list (build-path root "docs"))
                            #:on-progress void))
     (file-catalog-wait worker 10)
     (touch (build-path root "elsewhere" "leak.txt"))
     (check-false (reconcile-directory-in-catalog!
                  worker (build-path root "elsewhere")))
     (check-equal? (file-index-search db "leak" 8) '())
     (stop-file-catalog! worker))))

(test-case "the watcher API degrades off-Windows and works on Windows"
  (cond
    [(not (fs-watch-supported?))
     (check-false (fs-watch-supported?))
     (check-false (start-fs-watch! (find-system-path 'temp-dir) void void))]
    [else
     (check-true (fs-watch-supported?))
     (with-temp-tree
      (lambda (root)
        (define events (box '()))
        (define watch
          (start-fs-watch! root
                           (lambda (paths)
                             (set-box! events (append (unbox events) paths)))
                           void))
        (check-true (fs-watch? watch))
        (touch (build-path root "live-created.txt"))
        (let wait ([remaining 60])
          (cond
            [(memf (lambda (p) (string-contains? p "live-created.txt"))
                   (unbox events))
             (check-true #t)]
            [(zero? remaining) (fail-check "no watcher event in 3s")]
            [else (sleep 0.05) (wait (sub1 remaining))]))
        (stop-fs-watch! watch)
        (check-true (thread-dead? (fs-watch-thread watch))
                    "watch thread must exit on stop")))]))

(test-case "engine wiring: created files become searchable live (Windows)"
  (unless (fs-watch-supported?)
    (displayln "engine live wiring: skipped off-Windows (watcher no-op)"))
  (when (fs-watch-supported?)
    (with-temp-tree
     (lambda (root)
       (make-directory (build-path root "Documents"))
       (define db (build-path root "index.sqlite3"))
       (define nudges (box 0))
       (define engine
         (make-engine #:clipboard-store #f
                      #:snippet-store #f
                      #:plugin-manager #f
                      #:file-index-path db
                      #:file-catalog-roots
                      (list (build-path root "Documents"))
                      #:file-catalog-nudge
                      (lambda () (set-box! nudges (add1 (unbox nudges))))))
       (engine-start-file-catalog! engine)
       ;; Wait for the scan to complete and the watcher to arm.
       (let wait-ready ([n 100])
         (cond
           [(memq (file-catalog-status-state (engine-file-catalog-status engine))
                  '(ready partial)) (void)]
           [(zero? n) (fail-check "catalog did not finish")]
           [else (sleep 0.05) (wait-ready (sub1 n))]))
       ;; Create a file AFTER the watcher armed; no reconcile call is made
       ;; by the test — visibility must arrive through the live path.
       (touch (build-path root "Documents" "live-search.txt"))
       (let wait-live ([n 100])
         (define rows (engine-search engine "find live-search"))
         (cond
           [(memf (lambda (row) (string-contains? (cadr row) "live-search.txt"))
                  rows)
            (check-true (> (unbox nudges) 0)
                        "a search-revision nudge must repaint open queries")]
           [(zero? n) (fail-check "live visibility exceeded the test budget")]
           [else (sleep 0.05) (wait-live (sub1 n))]))
       ;; Delete → must leave the results through the same path.
       (delete-file (build-path root "Documents" "live-search.txt"))
       (let wait-gone ([n 100])
         (define rows (engine-search engine "find live-search"))
         (cond
           [(not (memf (lambda (row) (string-contains? (cadr row) "live-search.txt"))
                       rows)) (check-true #t)]
           [(zero? n)
            (fail-check
             (format "deletion visibility exceeded the test budget; nudges=~a rows=~s"
                     (unbox nudges)
                     (engine-search engine "find live-search")))]
           [else (sleep 0.05) (wait-gone (sub1 n))]))
       (engine-stop! engine)))))

(test-case "engine-stop! shuts the watcher down cleanly"
  (when (fs-watch-supported?)
    (with-temp-tree
     (lambda (root)
       (define db (build-path root "index.sqlite3"))
       (define engine
         (make-engine #:clipboard-store #f
                      #:snippet-store #f
                      #:plugin-manager #f
                      #:file-index-path db
                      #:file-catalog-roots (list root)
                      #:file-catalog-nudge void))
       (engine-start-file-catalog! engine)
       (let wait-ready ([n 100])
         (cond
           [(memq (file-catalog-status-state (engine-file-catalog-status engine))
                  '(ready partial)) (void)]
           [(zero? n) (fail-check "catalog did not finish")]
           [else (sleep 0.05) (wait-ready (sub1 n))]))
       (define live (engine-file-catalog-live engine))
       (check-true (and live (pair? (first live))))
       (define threads (map fs-watch-thread (first live)))
       (engine-stop! engine)
       (for ([t (in-list threads)])
         (check-true (thread-dead? t) "watch thread must exit on engine-stop!"))
       (check-false (engine-file-catalog-live engine))))))
