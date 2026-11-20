#lang racket/base

(require db
         racket/file
         racket/path
         rackunit
         "../app/core/file-index.rkt")

(define (with-index thunk)
  (define dir (make-temporary-file "fulcrum-file-index-~a" 'directory))
  (dynamic-wind
    void
    (lambda () (thunk (build-path dir "index.sqlite3")))
    (lambda () (delete-directory/files dir))))

(test-case "persistent index orders exact stem before prefix across path styles"
  (with-index
   (lambda (path)
     (check-true
      (file-index-upsert-hits!
       path
       '(('dummy . "ignored"))))
     ;; Invalid rows are ignored; real entries cover Windows, POSIX, Unicode,
     ;; and case-folded lookup without depending on the test host's path rules.
     (check-true
      (file-index-upsert-hits!
       path
       '(("Report-old.txt" . "C:\\Users\\me\\Report-old.txt")
         ("report.md" . "/home/me/report.md")
         ("REPORTING.xlsx" . "/home/me/REPORTING.xlsx")
         ("报告.md" . "D:\\资料\\报告.md"))))
     (check-equal? (file-index-count path) 4)
     (check-equal?
      (file-index-search path "report" 4)
      '(("report.md" . "/home/me/report.md")
        ("Report-old.txt" . "C:\\Users\\me\\Report-old.txt")
        ("REPORTING.xlsx" . "/home/me/REPORTING.xlsx")))
     (check-equal? (file-index-search path "报告" 8)
                   '(("报告.md" . "D:\\资料\\报告.md"))))))

(test-case "incremental add rename and remove survive reopen"
  (with-index
   (lambda (path)
     (file-index-upsert-hits!
      path
      '(("alpha.txt" . "/docs/alpha.txt")
        ("beta.txt" . "/docs/beta.txt")))
     (check-true
      (file-index-rename! path "/docs/alpha.txt" "C:\\Docs\\alpha-final.txt"))
     (check-equal? (file-index-search path "alpha" 8)
                   '(("alpha-final.txt" . "C:\\Docs\\alpha-final.txt")))
     (check-true (file-index-remove-paths! path '("/docs/beta.txt")))
     ;; Every API call opens a fresh connection, exercising persistent rather
     ;; than process-local state.
     (check-equal? (file-index-count path) 1)
     (check-equal? (file-index-search path "beta" 8) '()))))

(test-case "pinyin terms follow file add rename and remove"
  (with-index
   (lambda (path)
     (check-true
      (file-index-upsert-hits!
       path
       '(("微信计划.md" . "D:\\资料\\微信计划.md")
         ("臺灣行程.txt" . "/docs/臺灣行程.txt"))))
     (check-equal? (map car (file-index-search path "weixin" 8))
                   '("微信计划.md"))
     (check-equal? (map car (file-index-search path "wxjh" 8))
                   '("微信计划.md"))
     (check-equal? (map car (file-index-search path "taiwan" 8))
                   '("臺灣行程.txt"))
     (check-true
      (file-index-rename! path
                          "D:\\资料\\微信计划.md"
                          "D:\\资料\\钉钉计划.md"))
     (check-equal? (file-index-search path "weixin" 8) '())
     (check-equal? (map car (file-index-search path "dingding" 8))
                   '("钉钉计划.md"))
     (check-true (file-index-remove-paths! path '("D:\\资料\\钉钉计划.md")))
     (check-equal? (file-index-search path "dingding" 8) '()))))


(test-case "index errors are distinguishable from valid empty searches"
  (with-index
   (lambda (db)
     (define empty (file-index-search/status db "unmatched" 8))
     ;; An ordinary file cannot be a directory. This reproducibly denies
     ;; cache creation on Windows, macOS and Ubuntu without chmod assumptions.
     (define blocker (build-path (path-only db) "not-a-directory"))
     (define broken (build-path blocker "index.sqlite3"))
     (check-equal? (file-index-query-status empty) 'ok)
     (check-equal? (file-index-query-hits empty) '())
     (display-to-file "blocked" blocker)
     (let ([failed (file-index-search/status broken "report" 8)])
       (check-equal? (file-index-query-status failed) 'error)
       (check-equal? (file-index-query-hits failed) '()))
     ;; Legacy callers still get a harmless list, not an exception.
     (check-equal? (file-index-search broken "report" 8) '()))))

(test-case "corrupt cache is discarded and rebuilt safely"
  (with-index
   (lambda (path)
     (call-with-output-file path
       (lambda (out) (display "not a sqlite database" out))
       #:exists 'truncate)
     (check-equal? (file-index-count path) 0)
     (check-true
      (file-index-upsert-hits! path '(("recovered.md" . "/docs/recovered.md"))))
     (check-equal? (file-index-search path "recover" 8)
                   '(("recovered.md" . "/docs/recovered.md"))))))

(test-case "infix fallback finds substring matches the prefix tiers miss"
  (with-index
   (lambda (path)
     (file-index-upsert-hits!
      path
      '(("fulcrum_freshness_probe_20261120.txt"
         . "C:\\Users\\me\\Documents\\fulcrum_freshness_probe_20261120.txt")
        ("notepad-notes.md" . "/docs/notepad-notes.md")
        ("report.md" . "/docs/report.md")))
     ;; No name starts with "freshness"; the substring tier must find it
     ;; from the catalog itself, without the platform stage's crawl.
     (check-equal? (map car (file-index-search path "freshness" 8))
                   '("fulcrum_freshness_probe_20261120.txt"))
     (check-equal? (map car (file-index-search path "pad" 8))
                   '("notepad-notes.md"))
     ;; Prefix hits keep their priority; infix only supplements them.
     (check-equal? (map car (file-index-search path "note" 8))
                   '("notepad-notes.md"))
     ;; LIKE metacharacters match literally, not as wildcards.
     (check-equal? (file-index-search path "r%port" 8) '())
     (check-equal? (file-index-search path "repo_t" 8) '())
     ;; One character stays prefix-only: a %x% scan matches most of the
     ;; disk for no useful gain.
     (check-equal? (file-index-search path "e" 8) '()))))

(test-case "concurrent first open keeps the first scan root's rows"
  (with-index
   (lambda (db)
     (for ([i (in-range 6)])
       (define path (build-path (path-only db) (format "race-~a.sqlite3" i)))
       ;; Hold the write lock while two connections open a fresh database so
       ;; both read user_version 0 before either can migrate; releasing the
       ;; lock then forces the double migration the fix must survive. The
       ;; second migration used to drop the first root's committed rows
       ;; (packaged first launch lost its Desktop root exactly this way).
       (define holder (sqlite3-connect #:database path #:mode 'create))
       (query-exec holder "BEGIN IMMEDIATE")
       (define writer
         (thread
          (lambda ()
            (file-index-upsert-scan-hits!
             path "C:\\Users\\me\\Desktop" 1
             '(("desktop.ini" . "C:\\Users\\me\\Desktop\\desktop.ini")
               ("notes.pdf" . "C:\\Users\\me\\Desktop\\notes.pdf"))))))
       (define reader
         (thread (lambda () (file-index-search path "desktop" 8))))
       (sleep 0.25)
       (query-exec holder "ROLLBACK")
       (disconnect holder)
       (sync writer)
       (sync reader)
       (check-equal? (file-index-count path) 2)
       (check-equal? (map car (file-index-search path "notes" 8))
                     '("notes.pdf"))))))
