#lang racket/base

(require racket/file
         racket/list
         racket/string
         rackunit
         "../app/core/workflows.rkt")

(define (with-store proc)
  (define dir (make-temporary-file "fulcrum-workflow-test-~a" 'directory))
  (define path (build-path dir "workflows.json"))
  (dynamic-wind
    void
    (lambda () (proc (make-workflow-store path) path))
    (lambda () (delete-directory/files dir))))

(test-case "author, persist, branch, preview and copy via a bounded pipeline"
  (with-store
   (lambda (store path)
     (check-equal? (workflow-name (workflow-create! store "Morning")) "morning")
     (for ([step (in-list '("trim"
                            "ifcontains https ? upper : lower"
                            "prefix Result: "
                            "copy"))])
       (workflow-add-step! store "morning" step))
     (check-equal? (length (workflow-steps (workflow-ref store "morning"))) 4)
     (define dry
       (workflow-execute! store "morning" "  https://example.test  "
                          #:dry-run? #t))
     (check-equal? (workflow-outcome-status dry) "ok")
     (check-false (workflow-outcome-action dry))
     (check-equal? (workflow-outcome-text dry) "Result:HTTPS://EXAMPLE.TEST")
     (check-equal? (last (last (workflow-last-trace store "morning")))
                   "preview")
     (define run
       (workflow-execute! store "morning" "   hello WORLD   "))
     (check-equal? (workflow-outcome-status run) "ok")
     (check-equal? (workflow-outcome-action run) 'copy)
     (check-equal? (workflow-outcome-text run) "Result:hello world")
     (check-equal? (third (second (workflow-outcome-trace run))) "else")
     (define reopened (make-workflow-store path))
     (check-false (workflow-store-load-error? reopened))
     (check-equal? (length (workflow-steps (car (workflow-list reopened)))) 4)
     ;; Neither the input nor output nor trace can leak into workflows.json.
     (define disk (file->string path))
     (check-false (string-contains? disk "EXAMPLE.TEST"))
     (check-false (string-contains? disk "hello world"))
     (check-equal? (workflow-last-trace reopened "morning") '()))))

(test-case "failure stops subsequent copy/paste steps and records redacted trace"
  (with-store
   (lambda (store _)
     (workflow-create! store "gate")
     (workflow-add-step! store "gate" "require allowed")
     (workflow-add-step! store "gate" "paste")
     (define blocked (workflow-execute! store "gate" "top secret"))
     (check-equal? (workflow-outcome-status blocked) "failed")
     (check-false (workflow-outcome-action blocked))
     (check-equal? (length (workflow-outcome-trace blocked)) 1)
     (check-false (string-contains?
                   (format "~s" (workflow-outcome-trace blocked))
                   "top secret"))
     (check-equal? (workflow-outcome-action
                    (workflow-execute! store "gate" "allowed"))
                   'paste)
     (check-exn exn:fail?
                (lambda () (workflow-add-step! store "gate" "upper")))
     (workflow-remove-step! store "gate" 2)
     (workflow-add-step! store "gate" "upper")
     (check-equal? (length (workflow-steps (workflow-ref store "gate"))) 2))))

(test-case "invalid steps, names and too-long inputs never execute effects"
  (with-store
   (lambda (store _)
     (check-exn exn:fail? (lambda () (workflow-create! store "../escape")))
     (check-exn exn:fail? (lambda () (parse-workflow-step "shell rm -rf /")))
     (check-exn exn:fail? (lambda () (parse-workflow-step "prefix ")))
     (check-exn exn:fail? (lambda () (parse-workflow-step
                                     (string-append "prefix " (make-string 257 #\a)))))
     (workflow-create! store "safe")
     (workflow-add-step! store "safe" "copy")
     (check-equal? (workflow-outcome-status
                    (workflow-execute! store "safe" (make-string 8193 #\x)))
                   "failed")
     (check-exn exn:fail?
                (lambda () (workflow-remove-step! store "safe" 0)))
     (check-true (workflow-delete! store "safe"))
     (check-false (workflow-delete! store "safe")))))


(test-case "batch line transforms handle CRLF, filtering, stable dedup and sorting"
  (with-store
   (lambda (store path)
     (workflow-create! store "batch")
     (for ([step (in-list '("lines trim"
                            "lines drop #"
                            "lines unique"
                            "lines sort"
                            "lines take 3"
                            "copy"))])
       (workflow-add-step! store "batch" step))
     (define input "pear \r\n apple\r\npear\r\n# hidden\r\n plum")
     (define dry
       (workflow-execute! store "batch" input #:dry-run? #t))
     (check-equal? (workflow-outcome-status dry) "ok")
     (check-equal? (workflow-outcome-text dry) "apple\npear\nplum")
     (check-false (workflow-outcome-action dry))
     (define run (workflow-execute! store "batch" input))
     (check-equal? (workflow-outcome-action run) 'copy)
     (check-equal? (workflow-outcome-text run) "apple\npear\nplum")
     (check-equal? (length (workflow-outcome-trace run)) 6)
     (check-false (string-contains? (format "~s" (workflow-outcome-trace run))
                                    "pear"))
     (define restored (make-workflow-store path))
     (check-equal? (length (workflow-steps (workflow-ref restored "batch"))) 6))))

(test-case "line-mode subsets and failure recovery are explicitly traced"
  (with-store
   (lambda (store _)
     (workflow-create! store "subset")
     (workflow-add-step! store "subset" "lines upper")
     (workflow-add-step! store "subset" "lines keep OK")
     (workflow-add-step! store "subset" "lines nonempty")
     (workflow-add-step! store "subset" "lines lower")
     (workflow-add-step! store "subset" "require all-ok else fallback")
     (workflow-add-step! store "subset" "copy")
     (define result
       (workflow-execute! store "subset" "ok\nmissing\nok"))
     (check-equal? (workflow-outcome-text result) "fallback")
     (check-equal? (workflow-outcome-action result) 'copy)
     (check-equal? (third (list-ref (workflow-outcome-trace result) 4))
                   "recovered")
     (check-false (string-contains?
                   (format "~s" (workflow-outcome-trace result))
                   "fallback"))
     (workflow-create! store "strict")
     (workflow-add-step! store "strict" "require must-exist")
     (workflow-add-step! store "strict" "copy")
     (check-equal?
      (workflow-outcome-status (workflow-execute! store "strict" "absent"))
      "failed"))))

(test-case "batch and fallback grammar rejects invalid bounds or executable text"
  (with-store
   (lambda (store _)
     (for ([bad (in-list '("lines take 0" "lines take 1001"
                           "lines take 1.5" "lines keep"
                           "lines drop" "lines eval" "require x else"))])
       (check-exn exn:fail? (lambda () (parse-workflow-step bad))))
     (workflow-create! store "limit")
     (workflow-add-step! store "limit" "lines unique")
     (workflow-add-step! store "limit" "copy")
     (check-equal?
      (workflow-outcome-status
       (workflow-execute! store "limit" (make-string 8193 #\a)))
      "failed")
     (check-equal? (workflow-outcome-status
                    (workflow-execute! store "limit" "one\ntwo"))
                   "ok"))))

(test-case "malformed persisted store is read-only, not overwritten"
  (with-store
   (lambda (_store path)
     (display-to-file "{broken json" path #:exists 'truncate)
     (define broken (make-workflow-store path))
     (check-not-false (workflow-store-load-error? broken))
     (check-exn exn:fail? (lambda () (workflow-create! broken "new")))
     (check-equal? (file->string path) "{broken json"))))
