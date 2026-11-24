#lang racket/base

;; Settings import / export, JSON v2 (#241) — the prototype's contract
;; (design/prototype/settings.js `exportConfig` / `validateImport`).
;;
;; Four categories: `appearance` (General minus the hotkey), `features`
;; (every toggle and select on a Features page), `commands` (aliases and
;; hotkeys) and `content` (snippets and quicklinks). Never exported:
;; credentials, source paths, indexes, history, permission grants and the
;; startup preference.
;;
;; Import validates the whole file first and returns a plan — the groups it
;; would replace and how many entries each holds — so a host can show that
;; summary and ask before anything changes. Applying the plan replaces the
;; selected categories wholesale; unselected categories are untouched.
;;
;; A rejected file raises exn:fail whose message starts with a stable code
;; (format, size, value, conflict, empty) followed by ": " and a detail, so a
;; host can show its own localized message instead of parsing prose.

(require json
         racket/contract
         racket/format
         racket/list
         racket/port
         racket/string
         "bindings.rkt"
         "quicklinks.rkt"
         "settings.rkt"
         "snippets.rkt")

(provide transfer-categories
         settings-export-json
         settings-import-plan
         plan-summary
         transfer-apply!)

(define transfer-categories '(appearance features commands content))

(define import-max-bytes (* 1024 1024))
(define max-commands 200)
(define max-content-entries 500)

(define (transfer-exn code detail)
  (raise (make-exn:fail (format "~a: ~a" code detail)
                        (current-continuation-marks))))

;; ---- export ---------------------------------------------------------------

;; Typed value → JSON native: a toggle is a boolean, a number a number,
;; everything else the wire string.
(define (value->json key value)
  (define control (list-ref (list-ref (settings-spec key) 3) 2))
  (case control
    [(toggle) (and value #t)]
    [(number) value]
    [else (settings-value->wire value)]))

(define (preference-group manager category)
  (for/hasheq ([key (in-list (settings-exportable-keys category))])
    (values key
            (value->json key (settings-get manager key)))))

(define (quicklink-url-valid? url)
  (or (regexp-match? #px"^https?://" url)
      (regexp-match? #px"^(file://|~/|/|[A-Za-z]:[\\\\/])" url)))

(define (settings-export-json manager bindings snippets quicklinks categories)
  (define groups
    (append
     (for/list ([category (in-list '(appearance features))]
                #:when (memq category categories))
       (cons category (preference-group manager category)))
     (for/list ([_ (in-list '(commands))]
                #:when (memq 'commands categories))
       (cons 'commands
             (for/list ([b (in-list (bindings-list-all bindings))])
               (hasheq 'id (binding-id b)
                       'alias (or (binding-alias b) "")
                       'hotkey (or (binding-hotkey b) "")))))
     (for/list ([_ (in-list '(content))]
                #:when (memq 'content categories))
       (cons 'content
             (hasheq
              'snippets
              (for/list ([s (in-list (snippet-list snippets))])
                (hasheq 'name (snippet-name s)
                        'keyword (snippet-keyword s)
                        'text (snippet-text s)))
              'quicklinks
              (for/list ([l (in-list (link-list quicklinks))])
                (hasheq 'name (quicklink-name l)
                        'keyword (quicklink-keyword l)
                        'url (quicklink-url l))))))))
  (jsexpr->string (hasheq 'format "fulcrum-settings"
                          'version 2
                          'groups (apply hasheq
                                         (apply append
                                                (map (lambda (g) (list (car g) (cdr g)))
                                                     groups))))))

;; ---- validation -----------------------------------------------------------

;; JSON key → symbol (read-json hands back symbols; be liberal about
;; strings too).
(define (hash-ref* h key [default #f])
  (or (hash-ref h key #f)
      (and (symbol? key) (hash-ref h (symbol->string key) default))
      default))

;; Coerce an imported JSON value against the key's schema, then — for a
;; select — require one of its declared options.
(define (imported-value key v)
  (define wire
    (cond [(boolean? v) (if v "on" "off")]
          [(string? v) v]
          [else (format "~a" v)]))
  (define coerced (settings-coerce key wire))
  (define meta (list-ref (settings-spec key) 3))
  (unless (and (car coerced) meta)
    (transfer-exn "value" key))
  (define options (list-ref meta 3))
  (when (pair? options)
    (unless (member (settings-value->wire (cdr coerced))
                    (map (lambda (o) (format "~a" o)) options))
      (transfer-exn "value" key)))
  (cdr coerced))

(define (preference-plan manager category raw)
  (unless (hash? raw) (transfer-exn "format" category))
  (define allowed (settings-exportable-keys category))
  (for/hasheq ([(k v) (in-hash raw)])
    (define key (string->symbol (format "~a" k)))
    ;; A key the product does not export (or no longer has) is refused
    ;; rather than dropped silently: the file is not what it claims.
    (unless (memq key allowed) (transfer-exn "value" key))
    (values key (imported-value key v))))

(define (command-plan commands summon-hotkey)
  (unless (and (list? commands) (<= (length commands) max-commands))
    (transfer-exn "format" "commands"))
  (define seen-alias (make-hash))
  (define seen-hotkey (make-hash (list (cons summon-hotkey #t))))
  (for/list ([c (in-list commands)])
    (unless (hash? c)
      (transfer-exn "format" "commands"))
    (define id (hash-ref* c 'id))
    (define alias (or (hash-ref* c 'alias) ""))
    (define hotkey (or (hash-ref* c 'hotkey) ""))
    (unless (and (non-empty-string? id)
                 (or (string=? alias "") (valid-alias? alias))
                 (or (string=? hotkey "") (valid-hotkey-spec? hotkey)))
      (transfer-exn "format" "commands"))
    (when (and (non-empty-string? alias)
               (hash-has-key? seen-alias (string-downcase alias)))
      (transfer-exn "conflict" alias))
    (when (and (non-empty-string? hotkey) (hash-has-key? seen-hotkey hotkey))
      (transfer-exn "conflict" hotkey))
    (hash-set! seen-alias (string-downcase alias) #t)
    (hash-set! seen-hotkey hotkey #t)
    (list id (if (string=? alias "") #f alias)
          (if (string=? hotkey "") #f hotkey))))

(define (text-field h key)
  (define v (hash-ref* h key))
  (if (string? v) (string-trim v) ""))

(define (content-plan raw)
  (unless (hash? raw) (transfer-exn "format" "content"))
  (define (entries name valid?)
    (define list* (hash-ref* raw name '()))
    (unless (and (list? list*) (<= (length list*) max-content-entries))
      (transfer-exn "format" name))
    (define seen (make-hash))
    (for/list ([e (in-list list*)])
      (unless (hash? e) (transfer-exn "format" name))
      (define item (valid? e))
      (unless item (transfer-exn "format" name))
      (define keyword (cadr item))
      (when (and (non-empty-string? keyword)
                 (hash-has-key? seen (string-downcase keyword)))
        (transfer-exn "conflict" keyword))
      (hash-set! seen (string-downcase keyword) #t)
      item))
  (hasheq
   'snippets
   (entries 'snippets
            (lambda (e)
              (define name (text-field e 'name))
              (define keyword (text-field e 'keyword))
              (define text (hash-ref* e 'text))
              (and (non-empty-string? name)
                   (string? text) (non-empty-string? (string-trim text))
                   (list name text (if (string=? keyword "") #f keyword)))))
   'quicklinks
   (entries 'quicklinks
            (lambda (e)
              (define name (text-field e 'name))
              (define keyword (text-field e 'keyword))
              (define url (text-field e 'url))
              (and (non-empty-string? name)
                   (quicklink-url-valid? url)
                   (list name url (if (string=? keyword "") #f keyword)))))))

;; Parse and validate a payload. Returns an association list
;; ((category . payload) …) in category order; a file with no supported
;; group is rejected so an import never silently does nothing.
(define (settings-import-plan manager payload)
  (when (> (string-length payload) import-max-bytes)
    (transfer-exn "size" import-max-bytes))
  (define data
    (with-handlers ([exn:fail? (lambda (_) (transfer-exn "format" "not JSON"))])
      (with-input-from-string payload read-json)))
  (unless (and (hash? data)
               (equal? (hash-ref* data 'format) "fulcrum-settings")
               (equal? (hash-ref* data 'version) 2)
               (hash? (hash-ref* data 'groups)))
    (transfer-exn "format" "not a Fulcrum settings export (version 2)"))
  (define groups (hash-ref* data 'groups))
  (define plan
    (append
     (for/list ([category (in-list '(appearance features))]
                #:when (hash-has-key* groups category))
       (cons category (preference-plan manager category (hash-ref* groups category))))
     (for/list ([_ (in-list '(commands))]
                #:when (hash-has-key* groups 'commands))
       (cons 'commands
             (command-plan (hash-ref* groups 'commands)
                           (settings-get manager 'hotkey))))
     (for/list ([_ (in-list '(content))]
                #:when (hash-has-key* groups 'content))
       (cons 'content (content-plan (hash-ref* groups 'content))))))
  (when (null? plan) (transfer-exn "empty" "no supported groups"))
  plan)

(define (hash-has-key* h key)
  (or (hash-has-key? h key)
      (and (symbol? key) (hash-has-key? h (symbol->string key)))))

;; "<category>:<count>,…" — content counts "snippets+quicklinks".
(define (plan-summary plan)
  (string-join
   (for/list ([entry (in-list plan)])
     (format "~a:~a"
             (car entry)
             (cond
               [(eq? (car entry) 'content)
                (format "~a+~a"
                        (length (hash-ref (cdr entry) 'snippets))
                        (length (hash-ref (cdr entry) 'quicklinks)))]
               [(hash? (cdr entry)) (hash-count (cdr entry))]
               [else (length (cdr entry))])))
   ","))

;; ---- apply ----------------------------------------------------------------

;; after-set! is called with each preference key written so the caller can
;; make live consumers take effect (theme, max-results, …).
(define (transfer-apply! manager bindings snippets quicklinks plan after-set!)
  (for ([entry (in-list plan)])
    (case (car entry)
      [(appearance features)
       (for ([(key value) (in-hash (cdr entry))])
         (settings-set! manager key value)
         (after-set! key))]
      [(commands)
       (for ([b (in-list (bindings-list-all bindings))])
         (binding-clear! bindings (binding-id b)))
       (for ([c (in-list (cdr entry))])
         (binding-set! bindings (list-ref c 0) (list-ref c 1) (list-ref c 2)))]
      [(content)
       (for ([s (in-list (snippet-list snippets))])
         (snippet-delete! snippets (snippet-id s)))
       (for ([s (in-list (hash-ref (cdr entry) 'snippets))])
         (snippet-save! snippets (list-ref s 0) (list-ref s 1) (list-ref s 2)))
       (for ([l (in-list (link-list quicklinks))])
         (link-delete! quicklinks (quicklink-id l)))
       (for ([l (in-list (hash-ref (cdr entry) 'quicklinks))])
         (link-save! quicklinks (list-ref l 0) (list-ref l 1) (list-ref l 2)))])))
