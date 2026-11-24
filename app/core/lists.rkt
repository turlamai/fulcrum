#lang racket/base

;; Named lists the settings pages edit (#241): application folders,
;; applications hidden from search, applications excluded from clipboard
;; history, script folders and file ignore rules. One store, one atomic
;; JSON snapshot (`lists.json`), the same durability rule as every other
;; store.
;;
;; A list item is a name plus whatever the kind needs: a folder path, a
;; free-text extra field (ignore patterns), and an enabled flag. The store
;; validates shape and size only — what a path or pattern means belongs to
;; the consumer (the app index, the file catalog). Validation failures raise
;; `exn:fail` with a stable code word first ("duplicate-name: …") so a host
;; can show its own localized message instead of parsing English prose.
;;
;; Lists are device-local and never mirrored to the sync root: paths and
;; app names name this machine.

(require racket/contract
         racket/file
         racket/list
         racket/string
         "store.rkt")

;; The accessors are listed by name alongside the struct so a reader of
;; the backend sees plain functions in use there.
(provide (struct-out list-item)
         (struct-out list-store)
         list-kinds
         valid-list-kind?
         list-kind-has-path?
         make-list-store
         list-items
         list-set!
         list-remove!)

(struct list-item (id name path extra enabled) #:transparent)

(struct list-store (path lock items) #:mutable)

;; kind → (needs-path?)
(define kinds
  '(("app-folders" . #t)
    ("apps" . #f)
    ("clip-excluded" . #f)
    ("script-folders" . #t)
    ("ignore-rules" . #f)))

(define (list-kinds) (map car kinds))

(define (valid-list-kind? kind)
  (and (member kind (map car kinds)) #t))

(define (list-kind-has-path? kind)
  (cdr (assoc kind kinds string=?)))

(define max-items-per-kind 500)
(define max-name-chars 100)
(define max-path-chars 1000)
(define max-extra-chars 2000)

(define (code-exn code detail)
  (raise (make-exn:fail (format "~a: ~a" code detail) (current-continuation-marks))))

(define (clip-to s limit)
  (if (> (string-length s) limit) (substring s 0 limit) s))

(define (non-blank? s)
  (and (string? s) (non-empty-string? (string-trim s))))

;; ---- persistence ---------------------------------------------------------

(define (item->json item)
  (hasheq 'id (list-item-id item)
          'name (list-item-name item)
          'path (list-item-path item)
          'extra (list-item-extra item)
          'enabled (list-item-enabled item)))

(define (json->item entry)
  (and (hash? entry)
       (non-blank? (hash-ref entry 'name #f))
       (let ([id (hash-ref entry 'id #f)])
         (list-item (if (string? id) id (next-id!))
                    (hash-ref entry 'name)
                    (or (hash-ref entry 'path "") "")
                    (or (hash-ref entry 'extra "") "")
                    (not (eq? (hash-ref entry 'enabled #t) #f))))))

(define (load-items path)
  (define raw (read-json-file path (hash)))
  (for/hash ([kind (in-list (map car kinds))])
    (define entries (if (hash? raw) (hash-ref raw (string->symbol kind) '()) '()))
    (values kind
            (filter values
                    (for/list ([entry (in-list (if (list? entries) entries '()))])
                      (json->item entry))))))

(define (persist! store)
  (write-json-atomic!
   (list-store-path store)
   (for/hash ([(kind items) (in-hash (list-store-items store))])
     (values (string->symbol kind) (map item->json items)))))

(define (next-id!)
  (format "i~a-~a" (current-inexact-milliseconds) (random 4294967087)))

(define/contract (make-list-store path)
  (-> path? list-store?)
  (list-store path (make-semaphore 1) (load-items path)))

;; ---- queries -------------------------------------------------------------

(define/contract (list-items store kind)
  (-> list-store? string? (listof list-item?))
  (unless (valid-list-kind? kind) (code-exn "unknown-kind" kind))
  (sort (hash-ref (list-store-items store) kind '())
        string-ci<? #:key list-item-name))

;; ---- mutations -----------------------------------------------------------

;; An empty id creates; otherwise the item with that id is replaced in
;; place (keeping its position). Returns the saved item.
(define/contract (list-set! store kind id name path extra enabled)
  (-> list-store? string? string? string? string? string? boolean? list-item?)
  (unless (valid-list-kind? kind) (code-exn "unknown-kind" kind))
  (unless (non-blank? name) (code-exn "name-required" kind))
  (when (and (list-kind-has-path? kind) (not (non-blank? path)))
    (code-exn "path-required" name))
  (when (and (string=? kind "ignore-rules") (not (non-blank? extra)))
    (code-exn "pattern-required" name))
  (define trimmed-name (clip-to (string-trim name) max-name-chars))
  (define trimmed-path (clip-to (string-trim path) max-path-chars))
  (define items (hash-ref (list-store-items store) kind '()))
  (when (for/or ([item (in-list items)])
          (and (not (string=? (list-item-id item) id))
               ;; Ignore rules may share a name; every other kind is keyed
               ;; by it (an app or a folder listed twice would be a no-op).
               (not (string=? kind "ignore-rules"))
               (string-ci=? (list-item-name item) trimmed-name)))
    (code-exn "duplicate-name" trimmed-name))
  (define record
    (list-item (if (non-blank? id) id (next-id!))
               trimmed-name
               (if (list-kind-has-path? kind) trimmed-path "")
               (clip-to (string-trim extra) max-extra-chars)
               enabled))
  (call-with-semaphore
   (list-store-lock store)
   (lambda ()
     (define current (hash-ref (list-store-items store) kind '()))
     (define next
       (if (findf (lambda (i) (string=? (list-item-id i) (list-item-id record))) current)
           (map (lambda (i) (if (string=? (list-item-id i) (list-item-id record)) record i))
                current)
           (append current (list record))))
     (unless (<= (length next) max-items-per-kind)
       (code-exn "limit" max-items-per-kind))
     (set-list-store-items! store (hash-set (list-store-items store) kind next))
     (persist! store)
     record)))

(define/contract (list-remove! store kind id)
  (-> list-store? string? string? boolean?)
  (unless (valid-list-kind? kind) (code-exn "unknown-kind" kind))
  (call-with-semaphore
   (list-store-lock store)
   (lambda ()
     (define current (hash-ref (list-store-items store) kind '()))
     (define next (filter (lambda (i) (not (string=? (list-item-id i) id))) current))
     (define removed? (< (length next) (length current)))
     (when removed?
       (set-list-store-items! store (hash-set (list-store-items store) kind next))
       (persist! store))
     removed?)))
