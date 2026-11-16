#lang racket/base

;; Bounded, declarative workflows shared by all three native hosts.
;; No subprocess, network, script eval or hidden trigger is permitted here.
;; Workflow inputs/outputs never enter the persisted store or run traces.

(require racket/file
         racket/list
         racket/string
         "paths.rkt"
         "store.rkt")

(provide (struct-out workflow)
         (struct-out workflow-store)
         (struct-out workflow-outcome)
         make-workflow-store
         workflow-list
         workflow-ref
         workflow-create!
         workflow-add-step!
         workflow-remove-step!
         workflow-delete!
         workflow-execute!
         workflow-last-trace
         workflow-step-label
         parse-workflow-step)

(struct workflow (name steps) #:transparent)
(struct workflow-store (path items lock traces load-error?) #:mutable)
(struct workflow-outcome (status action text message trace) #:transparent)

(define max-workflows 50)
(define max-steps 20)
(define max-argument-chars 256)
(define max-input-chars 8192)
(define max-output-chars 65536)
(define format-name "fulcrum-workflows")

(define (normalize-name value)
  (string-downcase (string-trim value)))

(define (valid-name? value)
  (and (string? value)
       (regexp-match? #px"^[a-z][a-z0-9_-]{0,31}$" value)))

(define (bounded-text? text [allow-empty? #f])
  (and (string? text)
       (<= (string-length text) max-argument-chars)
       (or allow-empty? (not (string=? text "")))))

(define plain-ops '("trim" "upper" "lower" "copy" "paste"))
(define branches '("keep" "trim" "upper" "lower"))
(define line-ops '("trim" "upper" "lower" "unique" "sort" "nonempty"))

(define (terminal-step? step)
  (member (hash-ref step 'op "") '("copy" "paste")))

(define (valid-step? step)
  (and (hash? step)
       (let ([op (hash-ref step 'op #f)])
         (and (string? op)
              (cond
                [(member op plain-ops) #t]
                [(member op '("prefix" "suffix" "require"))
                 (bounded-text? (hash-ref step 'text #f))]
                [(string=? op "replace")
                 (and (bounded-text? (hash-ref step 'from #f))
                      (bounded-text? (hash-ref step 'to #f) #t))]
                [(string=? op "require-or")
                 (and (bounded-text? (hash-ref step 'text #f))
                      (bounded-text? (hash-ref step 'fallback #f) #t))]
                [(string=? op "lines")
                 (define mode (hash-ref step 'mode #f))
                 (and (string? mode)
                      (or (member mode line-ops)
                          (and (member mode '("keep" "drop"))
                               (bounded-text? (hash-ref step 'text #f)))
                          (and (string=? mode "take")
                               (exact-integer? (hash-ref step 'count #f))
                               (<= 1 (hash-ref step 'count) 1000))))]
                [(string=? op "ifcontains")
                 (and (bounded-text? (hash-ref step 'text #f))
                      (member (hash-ref step 'yes #f) branches)
                      (member (hash-ref step 'no #f) branches))]
                [else #f])))))

(define (valid-steps? steps)
  (and (list? steps)
       (<= (length steps) max-steps)
       (andmap valid-step? steps)
       (for/and ([step (in-list steps)]
                 [i (in-naturals)])
         (or (not (terminal-step? step))
             (= i (sub1 (length steps)))))))

;; One-line authoring grammar, avoiding a platform-specific editor in slice A.
;; e.g. wf add brief ifcontains https ? upper : keep
(define (parse-workflow-step description)
  (define text (string-trim description))
  (define plain (string-downcase text))
  (define parsed
    (cond
      [(member plain plain-ops) (hasheq 'op plain)]
      [(regexp-match #px"^prefix (.+)$" text)
       => (lambda (m) (hasheq 'op "prefix" 'text (cadr m)))]
      [(regexp-match #px"^suffix (.+)$" text)
       => (lambda (m) (hasheq 'op "suffix" 'text (cadr m)))]
      [(regexp-match #px"^require (.+) else (.+)$" text)
       => (lambda (m) (hasheq 'op "require-or"
                              'text (cadr m) 'fallback (caddr m)))]
      ;; Do not reinterpret an incomplete failure branch as an ordinary
      ;; require-text literal: the user explicitly typed the 'else' keyword.
      [(regexp-match? #px"^require .+ else$" text) #f]
      [(regexp-match #px"^require (.+)$" text)
       => (lambda (m) (hasheq 'op "require" 'text (cadr m)))]
      [(regexp-match #px"^lines (trim|upper|lower|unique|sort|nonempty)$" plain)
       => (lambda (m) (hasheq 'op "lines" 'mode (cadr m)))]
      [(regexp-match #px"^lines (keep|drop) (.+)$" text)
       => (lambda (m) (hasheq 'op "lines"
                              'mode (cadr m) 'text (caddr m)))]
      [(regexp-match #px"^lines take ([0-9]+)$" text)
       => (lambda (m) (hasheq 'op "lines" 'mode "take"
                              'count (string->number (cadr m))))]
      [(regexp-match #px"^replace (.+) => (.*)$" text)
       => (lambda (m) (hasheq 'op "replace"
                              'from (cadr m) 'to (caddr m)))]
      [(regexp-match
        #px"^ifcontains (.+) \\? (keep|trim|upper|lower) : (keep|trim|upper|lower)$"
        text)
       => (lambda (m) (hasheq 'op "ifcontains"
                              'text (cadr m) 'yes (caddr m)
                              'no (cadddr m)))]
      [else #f]))
  (unless (and parsed (valid-step? parsed))
    (raise-user-error
     'workflow-step
     "invalid step: use trim/upper/lower, prefix/suffix TEXT, replace OLD => NEW, require TEXT [else FALLBACK], lines trim/upper/lower/unique/sort/nonempty/keep TEXT/drop TEXT/take N, ifcontains TEXT ? upper : keep, copy or paste"))
  parsed)

(define (workflow-step-label step)
  (define op (hash-ref step 'op))
  (cond
    [(member op '("prefix" "suffix" "require"))
     (format "~a ~a" op (hash-ref step 'text))]
    [(string=? op "replace")
     (format "replace ~a => ~a" (hash-ref step 'from) (hash-ref step 'to))]
    [(string=? op "ifcontains")
     (format "ifcontains ~a ? ~a : ~a"
             (hash-ref step 'text) (hash-ref step 'yes) (hash-ref step 'no))]
    [(string=? op "require-or")
     (format "require ~a else ~a"
             (hash-ref step 'text) (hash-ref step 'fallback))]
    [(string=? op "lines")
     (define mode (hash-ref step 'mode))
     (cond [(member mode '("keep" "drop"))
            (format "lines ~a ~a" mode (hash-ref step 'text))]
           [(string=? mode "take")
            (format "lines take ~a" (hash-ref step 'count))]
           [else (format "lines ~a" mode)])]
    [else op]))

(define (read-workflows path)
  (define absent (gensym 'absent))
  (define raw (read-json-file path absent))
  (cond
    [(and (eq? raw absent) (not (file-exists? path)))
     (values (hash) #f)]
    [(and (hash? raw)
          (equal? (hash-ref raw 'format #f) format-name)
          (equal? (hash-ref raw 'version #f) 1)
          (list? (hash-ref raw 'workflows #f))
          (<= (length (hash-ref raw 'workflows)) max-workflows))
     (define entries (hash-ref raw 'workflows))
     (define items (make-hash))
     (define all-valid?
       (for/and ([entry (in-list entries)])
         (define name (and (hash? entry) (hash-ref entry 'name #f)))
         (define steps (and (hash? entry) (hash-ref entry 'steps #f)))
         (and (valid-name? name)
              (valid-steps? steps)
              (not (hash-has-key? items name))
              (begin
                (hash-set! items name (workflow name steps))
                #t))))
     (if all-valid?
         (values (make-immutable-hash (hash->list items)) #f)
         (values (hash) "invalid or duplicated workflow definition"))]
    [else (values (hash) "unreadable or unsupported workflows.json")]))
 
(define (make-workflow-store [path (workflows-path)])
  (define-values (items error-message) (read-workflows path))
  (workflow-store path items (make-semaphore 1) (make-hash) error-message))

(define (check-writable! store)
  (when (workflow-store-load-error? store)
    (raise-user-error 'workflow
                      (string-append
                       (workflow-store-load-error? store)
                       "; back up or repair the file before making changes"))))

(define (write-next! store next)
  (check-writable! store)
  (write-json-atomic!
   (workflow-store-path store)
   (hasheq 'format format-name
           'version 1
           'workflows
           (for/list ([w (in-list (sort (hash-values next) string<?
                                         #:key workflow-name))])
             (hasheq 'name (workflow-name w)
                     'steps (workflow-steps w)))))
  (set-workflow-store-items! store next))

(define (workflow-list store)
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda () (sort (hash-values (workflow-store-items store))
                    string<? #:key workflow-name))))

(define (workflow-ref store name)
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda () (hash-ref (workflow-store-items store)
                         (normalize-name name) #f))))

(define (workflow-create! store value)
  (define name (normalize-name value))
  (unless (valid-name? name)
    (raise-user-error 'workflow "name must be 1-32 lowercase letters, numbers, - or _, starting with a letter"))
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda ()
     (define current (workflow-store-items store))
     (when (hash-has-key? current name)
       (raise-user-error 'workflow "name is already in use"))
     (when (>= (hash-count current) max-workflows)
       (raise-user-error 'workflow "maximum of 50 workflows reached"))
     (define w (workflow name '()))
     (write-next! store (hash-set current name w))
     w)))

(define (workflow-add-step! store name description)
  (define step (parse-workflow-step description))
  (define slug (normalize-name name))
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda ()
     (define current (workflow-store-items store))
     (define old (hash-ref current slug #f))
     (unless old (raise-user-error 'workflow "no such workflow"))
     (define updated (append (workflow-steps old) (list step)))
     (unless (valid-steps? updated)
       (raise-user-error 'workflow "maximum 20 steps; copy/paste must be the final step"))
     (define w (workflow slug updated))
     (write-next! store (hash-set current slug w))
     w)))

(define (workflow-remove-step! store name index)
  (define slug (normalize-name name))
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda ()
     (define current (workflow-store-items store))
     (define old (hash-ref current slug #f))
     (unless old (raise-user-error 'workflow "no such workflow"))
     (unless (and (exact-positive-integer? index)
                  (<= index (length (workflow-steps old))))
       (raise-user-error 'workflow "step index is out of bounds"))
     (define updated
       (for/list ([step (in-list (workflow-steps old))]
                  [i (in-naturals 1)]
                  #:unless (= i index))
         step))
     (define w (workflow slug updated))
     (write-next! store (hash-set current slug w))
     w)))

(define (workflow-delete! store name)
  (define slug (normalize-name name))
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda ()
     (define current (workflow-store-items store))
     (if (hash-has-key? current slug)
         (begin
           (write-next! store (hash-remove current slug))
           (hash-remove! (workflow-store-traces store) slug)
           #t)
         #f))))

(define (workflow-last-trace store name)
  (call-with-semaphore
   (workflow-store-lock store)
   (lambda () (hash-ref (workflow-store-traces store)
                         (normalize-name name) '()))))

(define (apply-text-transform op value)
  (cond
    [(string=? op "keep") value]
    [(string=? op "trim") (string-trim value)]
    [(string=? op "upper") (string-upcase value)]
    [(string=? op "lower") (string-downcase value)]
    [else (error 'workflow "unknown pure transform")]))

;; Platform-independent line transformations. Preserve line order for
;; filters/deduplication and normalize CRLF before splitting so persisted
;; workflow definitions behave identically on macOS, Windows and Linux.
(define (transform-lines step input)
  (define mode (hash-ref step 'mode))
  (define lines (string-split (string-replace input "\r\n" "\n")
                              "\n" #:trim? #f))
  (define transformed
    (cond
      [(string=? mode "trim") (map string-trim lines)]
      [(string=? mode "upper") (map string-upcase lines)]
      [(string=? mode "lower") (map string-downcase lines)]
      [(string=? mode "nonempty")
       (filter (lambda (line) (not (string=? (string-trim line) ""))) lines)]
      [(string=? mode "unique") (remove-duplicates lines string=?)]
      [(string=? mode "sort") (sort lines string-ci<?)]
      [(string=? mode "keep")
       (filter (lambda (line)
                 (string-contains? line (hash-ref step 'text))) lines)]
      [(string=? mode "drop")
       (filter (lambda (line)
                 (not (string-contains? line (hash-ref step 'text)))) lines)]
      [(string=? mode "take")
       (take lines (min (hash-ref step 'count) (length lines)))]
      [else (error 'workflow "unvalidated line transform")]))
  (string-join transformed "\n"))

(define (execute-steps steps input dry?)
  (let loop ([rest steps] [index 1] [value input]
             [action #f] [trace '()])
    (cond
      [(null? rest)
       (workflow-outcome
        "ok" (and (not dry?) action) value
        (format "~a (~a step~a); run wf trace to inspect"
                (if dry? "Dry run passed" "Workflow completed")
                (length steps) (if (= (length steps) 1) "" "s"))
        (reverse trace))]
      [else
       (define step (car rest))
       (define op (hash-ref step 'op))
       (define (mark state) (cons (list index op state) trace))
       (cond
         [(and (string=? op "require")
               (not (string-contains? value (hash-ref step 'text))))
          (workflow-outcome
           "failed" #f "" (format "Step ~a failed: required text is absent" index)
           (reverse (mark "failed")))]
         [else
          (define-values (next next-action state)
            (cond
              [(member op '("trim" "upper" "lower"))
               (values (apply-text-transform op value) action "ok")]
              [(string=? op "prefix")
               (values (string-append (hash-ref step 'text) value) action "ok")]
              [(string=? op "suffix")
               (values (string-append value (hash-ref step 'text)) action "ok")]
              [(string=? op "replace")
               (values (string-replace value (hash-ref step 'from)
                                       (hash-ref step 'to)) action "ok")]
              [(string=? op "require") (values value action "ok")]
              [(string=? op "require-or")
               (if (string-contains? value (hash-ref step 'text))
                   (values value action "ok")
                   (values (hash-ref step 'fallback) action "recovered"))]
              [(string=? op "lines")
               (values (transform-lines step value) action "ok")]
              [(string=? op "ifcontains")
               (define yes? (string-contains? value (hash-ref step 'text)))
               (values (apply-text-transform
                        (hash-ref step (if yes? 'yes 'no)) value)
                       action (if yes? "then" "else"))]
              [(member op '("copy" "paste"))
               (values value (string->symbol op)
                       (if dry? "preview" "ok"))]
              [else (error 'workflow "unvalidated step")]))
          (if (> (string-length next) max-output-chars)
              (workflow-outcome
               "failed" #f ""
               (format "Step ~a failed: output exceeds 65536 characters" index)
               (reverse (mark "failed")))
              (loop (cdr rest) (add1 index) next next-action
                    (mark state)))])])))

(define (workflow-execute! store name input #:dry-run? [dry? #f])
  (define w (workflow-ref store name))
  (define outcome
    (cond
      [(not w) (workflow-outcome "failed" #f "" "No such workflow" '())]
      [(not (string? input))
       (workflow-outcome "failed" #f "" "Workflow input must be text" '())]
      [(> (string-length input) max-input-chars)
       (workflow-outcome "failed" #f "" "Input exceeds 8192 characters" '())]
      [(null? (workflow-steps w))
       (workflow-outcome "failed" #f "" "Workflow has no steps" '())]
      [else (execute-steps (workflow-steps w) input dry?)]))
  (when w
    (call-with-semaphore
     (workflow-store-lock store)
     (lambda ()
       (hash-set! (workflow-store-traces store)
                  (workflow-name w)
                  (workflow-outcome-trace outcome)))))
  outcome)
