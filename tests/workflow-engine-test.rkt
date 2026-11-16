#lang racket/base

(require racket/file
         racket/list
         racket/string
         rackunit
         "../app/core/engine.rkt"
         "../app/core/workflows.rkt")

(define dir (make-temporary-file "fulcrum-workflow-engine-~a" 'directory))
(define env (environment-variables-copy (current-environment-variables)))
(environment-variables-set! env #"FULCRUM_DATA_DIR"
                            (string->bytes/utf-8 (path->string dir)))

(dynamic-wind
  void
  (lambda ()
    (parameterize ([current-environment-variables env])
      (define store (make-workflow-store (build-path dir "workflows.json")))
      (define engine (make-engine #:workflow-store store #:plugin-manager #f))
      (define (action-for query)
        (define row (for/first ([r (in-list (engine-search engine query))]
                                #:when (string-prefix? (car r) "workflow."))
                      r))
        (check-not-false row (format "no workflow action row for ~a" query))
        (engine-run engine (first row) (list-ref row 4)))
      (check-equal? (car (action-for "wf create daily")) "stay")
      (check-true (for/or ([r (in-list (engine-search engine "workflow list"))])
                    (string-contains? (second r) "daily")))
      (for ([step (in-list '("trim" "ifcontains safe ? upper : lower" "copy"))])
        (check-equal? (car (action-for (format "wf add daily ~a" step))) "stay"))
      (check-equal? (length (filter (lambda (r) (equal? (list-ref r 3) "Workflow"))
                                    (engine-search engine "wf show daily"))) 3)
      (define dry (action-for "wf dry daily safe"))
      (check-equal? (car dry) "stay")
      (check-false (assoc 'copy-to-clipboard (cdr dry)))
      (define ran (action-for "wf run daily safe"))
      (check-equal? (car ran) "copied")
      (check-equal? (assoc 'copy-to-clipboard (cdr ran))
                    '(copy-to-clipboard . "SAFE"))
      (define trace
        (filter (lambda (r) (equal? (list-ref r 3) "Workflow"))
                (engine-search engine "wf trace daily")))
      (check-equal? (length trace) 3)
      (check-false (string-contains? (format "~s" trace) "SAFE"))
      (check-equal? (car (action-for "wf remove daily 2")) "stay")
      (check-equal? (length (filter (lambda (r) (equal? (list-ref r 3) "Workflow"))
                                    (engine-search engine "wf show daily"))) 2)
      (check-equal? (car (action-for "wf delete daily")) "stay")
      (check-false (workflow-ref store "daily"))))
  (lambda () (delete-directory/files dir)))
