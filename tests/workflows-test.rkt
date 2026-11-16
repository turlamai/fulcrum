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


(test-case "edit and reorder are atomic, persistent and invalidate old traces"
  (with-store
   (lambda (store path)
     (workflow-create! store "pipeline")
     (for ([step '("prefix A" "suffix B" "trim" "copy")])
       (workflow-add-step! store "pipeline" step))
     (define original (workflow-execute! store "pipeline" "x"))
     (check-equal? (workflow-outcome-text original) "AxB")
     (check-equal? (length (workflow-last-trace store "pipeline")) 4)
     (workflow-move-step! store "pipeline" 1 2)
     (check-equal? (workflow-last-trace store "pipeline") '())
     (check-equal?
      (map workflow-step-label (workflow-steps (workflow-ref store "pipeline")))
      '("suffix B" "prefix A" "trim" "copy"))
     (workflow-edit-step! store "pipeline" 2 "prefix Z")
     (define updated (workflow-execute! store "pipeline" "x"))
     (check-equal? (workflow-outcome-text updated) "ZxB")
     (check-equal? (workflow-outcome-action updated) 'copy)
     (check-equal? (length (workflow-last-trace store "pipeline")) 4)
     (define reopened (make-workflow-store path))
     (check-equal?
      (map workflow-step-label (workflow-steps (workflow-ref reopened "pipeline")))
      '("suffix B" "prefix Z" "trim" "copy"))
     (workflow-remove-step! store "pipeline" 3)
     (check-equal? (workflow-last-trace store "pipeline") '()))))

(test-case "terminal copy/paste invariant holds for edit and move failures"
  (with-store
   (lambda (store path)
     (workflow-create! store "strict")
     (for ([step '("trim" "upper" "copy")])
       (workflow-add-step! store "strict" step))
     (define initial (file->string path))
     (for ([attempt (list
                     (lambda () (workflow-edit-step! store "strict" 1 "paste"))
                     (lambda () (workflow-move-step! store "strict" 3 1))
                     (lambda () (workflow-move-step! store "strict" 1 3))
                     (lambda () (workflow-edit-step! store "strict" 4 "trim"))
                     (lambda () (workflow-move-step! store "strict" 0 1))
                     (lambda () (workflow-move-step! store "strict" 1 9))
                     (lambda () (workflow-edit-step! store "strict" 2 "shell uname")))])
       (check-exn exn:fail? attempt)
       (check-equal? (file->string path) initial))
     ;; A change to the terminal step is allowed at its only valid position.
     (workflow-edit-step! store "strict" 3 "paste")
     (check-equal? (workflow-outcome-action
                    (workflow-execute! store "strict" "a")) 'paste)
     (workflow-edit-step! store "strict" 3 "copy")
     (check-equal? (workflow-outcome-action
                    (workflow-execute! store "strict" "a")) 'copy))))

(test-case "no-op move retains trace, editing a step clears previous trace"
  (with-store
   (lambda (store _)
     (workflow-create! store "demo")
     (workflow-add-step! store "demo" "trim")
     (workflow-execute! store "demo" " value ")
     (check-equal? (length (workflow-last-trace store "demo")) 1)
     (workflow-move-step! store "demo" 1 1)
     (check-equal? (length (workflow-last-trace store "demo")) 1)
     (workflow-edit-step! store "demo" 1 "upper")
     (check-equal? (workflow-last-trace store "demo") '()))))

(test-case "malformed persisted store is read-only, not overwritten"
  (with-store
   (lambda (_store path)
     (display-to-file "{broken json" path #:exists 'truncate)
     (define broken (make-workflow-store path))
     (check-not-false (workflow-store-load-error? broken))
     (check-exn exn:fail? (lambda () (workflow-create! broken "new")))
     (check-equal? (file->string path) "{broken json"))))
