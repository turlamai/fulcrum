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

(test-case "malformed persisted store is read-only, not overwritten"
  (with-store
   (lambda (_store path)
     (display-to-file "{broken json" path #:exists 'truncate)
     (define broken (make-workflow-store path))
     (check-not-false (workflow-store-load-error? broken))
     (check-exn exn:fail? (lambda () (workflow-create! broken "new")))
     (check-equal? (file->string path) "{broken json"))))
