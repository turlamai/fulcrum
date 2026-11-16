#lang racket/base

(require racket/file
         racket/list
         racket/string
         rackunit
         "../app/core/clipboard.rkt"
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
      (define clip (make-clipboard-store (build-path dir "clipboard.json")))
      (define engine
        (make-engine #:clipboard-store clip
                     #:snippet-store #f
                     #:plugin-manager #f
                     #:workflow-store store))
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
      ;; A pinned OLD item must not replace the latest clipboard source.
      (define older (clipboard-record! clip "old pinned text"))
      (clipboard-toggle-pin! clip (clipboard-item-id older))
      (sleep 0.01)
      (clipboard-record! clip " alpha\r\n beta\r\n alpha ")
      (check-equal? (clipboard-item-text (clipboard-latest clip))
                    " alpha\r\n beta\r\n alpha ")
      (check-equal? (car (action-for "wf create batch")) "stay")
      (for ([step (in-list '("lines trim" "lines unique" "copy"))])
        (check-equal? (car (action-for (format "wf add batch ~a" step)))
                      "stay"))
      (define batch-run (action-for "wf run batch @clipboard"))
      (check-equal? (car batch-run) "copied")
      (check-equal? (assoc 'copy-to-clipboard (cdr batch-run))
                    '(copy-to-clipboard . "alpha\nbeta"))
      (define batch-dry (action-for "wf dry batch @clipboard"))
      (check-equal? (car batch-dry) "stay")
      (check-false (assoc 'copy-to-clipboard (cdr batch-dry)))
      (clipboard-record! clip "sk-abcdefghijklmnop123456")
      (define blocked (action-for "wf run batch @clipboard"))
      (check-equal? (car blocked) "stay")
      (check-false (assoc 'copy-to-clipboard (cdr blocked)))
      (check-false (string-contains? (format "~s" blocked)
                                     "sk-abcdefghijklmnop123456"))
      (check-equal? (car (action-for "wf run batch @@clipboard")) "copied")
      (check-equal? (assoc 'copy-to-clipboard
                           (cdr (action-for "wf run batch @@clipboard")))
                    '(copy-to-clipboard . "@clipboard"))
      ;; The native editor pages through all definitions, steps, and traces,
      ;; even when max-results is configured at its five-row minimum.
      (for ([i (in-range 1 13)])
        (check-equal?
         (car (action-for (format "wf create sample~a" i))) "stay"))
      (define (wf-rows query badge)
        (filter (lambda (row) (and (equal? (list-ref row 3) "Workflow")
                                   (equal? (list-ref row 7) badge)))
                (engine-search engine query)))
      ;; daily + batch + twelve sample definitions = fourteen.
      (check-equal? (length (wf-rows "wf list 1" "Saved")) 5)
      (check-equal? (length (wf-rows "wf list 2" "Saved")) 5)
      (check-equal? (length (wf-rows "wf list 3" "Saved")) 4)
      (for ([i (in-range 12)])
        (check-equal? (car (action-for "wf add sample1 trim")) "stay"))
      (check-equal? (length (wf-rows "wf show sample1 1" "Step")) 5)
      (check-equal? (length (wf-rows "wf show sample1 2" "Step")) 5)
      (check-equal? (length (wf-rows "wf show sample1 3" "Step")) 2)
      (check-equal? (list-ref (first (wf-rows "wf show sample1 3" "Step")) 1)
                    "sample1 · Step 11")
      (action-for "wf dry sample1 hello")
      (check-equal? (length (wf-rows "wf trace sample1 1" "Trace")) 5)
      (check-equal? (length (wf-rows "wf trace sample1 3" "Trace")) 2)
      (check-false
       (string-contains? (format "~s" (wf-rows "wf trace sample1 3" "Trace"))
                         "hello"))
      ;; Edit and move are first-class launcher actions, shared with all
      ;; three native editors through the same workflow action contract.
      (check-equal? (car (action-for "wf edit sample1 2 upper")) "stay")
      (check-equal? (car (action-for "wf move sample1 2 1")) "stay")
      (check-equal? (list-ref (first (wf-rows "wf show sample1 1" "Step")) 2)
                    "upper")
      (check-equal? (workflow-last-trace store "sample1") '())
      (action-for "wf dry sample1 hello")
      (check-equal? (length (workflow-last-trace store "sample1")) 12)
      (check-equal? (car (action-for "wf edit sample1 12 copy")) "stay")
      (define denied (action-for "wf move sample1 12 1"))
      (check-equal? (car denied) "stay")
      (check-true (string-contains?
                   (format "~s" (cdr denied))
                   "copy/paste must remain the final step"))
      (check-equal? (list-ref (last (wf-rows "wf show sample1 3" "Step")) 2)
                    "copy")
      (check-equal? (workflow-last-trace store "sample1") '())
      (check-equal? (car (action-for "wf delete daily")) "stay")
      (check-false (workflow-ref store "daily"))))
  (lambda () (delete-directory/files dir)))
