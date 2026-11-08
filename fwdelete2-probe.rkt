#lang racket/base
(require racket/file racket/format racket/string racket/list racket/path)
(require "app/core/engine.rkt" "app/core/file-catalog.rkt"
         "app/core/file-index.rkt" "app/core/fs-watch.rkt"
         "app/core/long-path.rkt")
(define (say . xs) (apply printf xs) (flush-output))
(define (parent-directory-of path-string)
  (define-values (base name dir?) (split-path (if (string? path-string) (string->path path-string) path-string)))
  (and (path? base) (path->string base)))
(define root (path->string (make-temporary-file "fwdel2~a" 'directory)))
(make-directory (build-path root "Documents"))
(define db (build-path root "index.sqlite3"))
(define nudges (box 0))
(define engine
  (make-engine #:clipboard-store #f #:snippet-store #f #:plugin-manager #f
               #:file-index-path db
               #:file-catalog-roots (list (build-path root "Documents"))
               #:file-catalog-nudge (lambda () (set-box! nudges (add1 (unbox nudges))))))
(engine-start-file-catalog! engine)
(let wait-ready ([n 200])
  (define st (file-catalog-status-state (engine-file-catalog-status engine)))
  (cond [(memq st '(ready partial)) (say "ready\n")]
        [(zero? n) (say "never ready\n")]
        [else (sleep 0.05) (wait-ready (sub1 n))]))
;; A logging watcher on the same dir the engine watches.
(define wroot (string-append (path->string (long-path (build-path root "Documents"))) "\\"))
(define log-w (start-fs-watch! wroot
                               (lambda (paths) (say "watcher events: ~s\n" paths))
                               (lambda (r) (say "watcher overflow: ~a\n" r))))
(say "watcher root: ~s\n" wroot)
;; What would containing see?
(define gens
  (file-catalog-worker-root-generations (engine-file-catalog-worker engine)))
(say "root-generations keys: ~s\n" (for/list ([k (in-hash-keys gens)]) k))
(display-to-file "x" (build-path root "Documents" "live-search.txt") #:exists 'truncate)
(let wait-live ([n 200])
  (define rows (engine-search engine "find live-search"))
  (cond [(memf (lambda (row) (string-contains? (cadr row) "live-search.txt")) rows)
         (say "created visible; nudges=~a\n" (unbox nudges))]
        [(zero? n) (say "creation never visible\n")]
        [else (sleep 0.05) (wait-live (sub1 n))]))
(say "deleting...\n")
(delete-file (build-path root "Documents" "live-search.txt"))
(sleep 3)
(say "after 3s: nudges=~a rows-present=~a\n"
     (unbox nudges)
     (not (null? (memf (lambda (row) (and (string? (cadr row)) (string-contains? (cadr row) "live-search.txt")))
                       (engine-search engine "find live-search")))))
(say "rows: ~s\n" (engine-search engine "find live-search"))
(say "done\n")
