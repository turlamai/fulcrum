#lang racket/base

;; Clipboard history store.
;;
;; Native hosts watch the OS clipboard and push every change through the
;; clipboard-record RPC; the backend owns history, deduplication, pinning,
;; search, and persistence. Content never leaves the device and is never
;; logged. The in-memory list is authoritative; the JSON file is a snapshot
;; rewritten atomically after each mutation.

(require racket/contract
         racket/list
         racket/string
         "paths.rkt"
         "store.rkt")

(provide (struct-out clipboard-item)
         (struct-out clipboard-store)
         make-clipboard-store
         clipboard-record!
         clipboard-remove!
         clipboard-toggle-pin!
         clipboard-clear!
         clipboard-item-ref
         clipboard-list
         clipboard-search
         clipboard-count
         sensitive-text?
         mask-sensitive-preview)

(struct clipboard-item (id text ts pinned? sensitive?) #:transparent)

(struct clipboard-store (path limit items lock) #:mutable)

(define max-item-chars 100000)

;; Sensitive-content detection (design v2 invariant 9): clipboard rows whose
;; content looks like a credential are masked on the wire and revealed only
;; through the local-only clipboard-reveal RPC. The patterns are deliberately
;; conservative — structured provider tokens, PEM headers, and long secret
;; assignments — so ordinary text is never hidden behind a mask.
(define sensitive-patterns
  (list
   #px"sk-[A-Za-z0-9_-]{16,}"
   #px"ghp_[A-Za-z0-9]{20,}"
   #px"gho_[A-Za-z0-9]{20,}"
   #px"github_pat_[A-Za-z0-9_]{20,}"
   #px"AKIA[0-9A-Z]{16}"
   #px"xox[baprs]-[A-Za-z0-9-]{10,}"
   #px"AIza[0-9A-Za-z_-]{30,}"
   #px"Bearer [A-Za-z0-9._~+/-]{20,}={0,2}"
   #rx"-----BEGIN [A-Z ]*PRIVATE KEY-----"
   #px"(?i:api[_-]?key|secret|password|passwd|access[_-]?token)[\"']?\\s*[:=]\\s*[\"']?[A-Za-z0-9+/_.=-]{16,}"))

(define/contract (sensitive-text? text)
  (-> string? boolean?)
  (for/or ([rx (in-list sensitive-patterns)]) (regexp-match? rx text)))

;; Masked preview: keep a short head so the user can still tell entries
;; apart; the bullets carry the meaning, not a colour.
(define mask-chars "••••••••••••")

(define/contract (mask-sensitive-preview text)
  (-> string? string?)
  (string-append (substring text 0 (min 4 (string-length text))) mask-chars))

(define (load-items path limit)
  (define raw (read-json-file path '()))
  (for/list ([entry (in-list (if (list? raw) raw '()))]
             #:when (and (hash? entry)
                         (hash-has-key? entry 'id)
                         (hash-has-key? entry 'text))
             #:unless (ormap eof-object? (list (hash-ref entry 'id #f)
                                               (hash-ref entry 'text #f))))
    (clipboard-item (hash-ref entry 'id)
                    (hash-ref entry 'text)
                    (or (hash-ref entry 'ts #f) 0)
                    (if (hash-ref entry 'pinned #f) #t #f)
                    ;; Legacy snapshots carry no flag: recompute on load so
                    ;; older history gets the same masking discipline.
                    (sensitive-text? (hash-ref entry 'text)))))

(define (persist! store)
  (write-json-atomic!
   (clipboard-store-path store)
   (for/list ([item (in-list (clipboard-store-items store))])
     (hasheq 'id (clipboard-item-id item)
             'text (clipboard-item-text item)
             'ts (clipboard-item-ts item)
             'pinned (clipboard-item-pinned? item)
             'sensitive (clipboard-item-sensitive? item)))))

(define (next-id!)
  (format "c~a-~a" (current-inexact-milliseconds) (random 4294967087)))

(define (enforce-limit! store)
  (define limit (clipboard-store-limit store))
  (define items (clipboard-store-items store))
  (when (> (length items) limit)
    (set-clipboard-store-items!
     store
     (append
      (filter clipboard-item-pinned? items)
      (take (filter (lambda (i) (not (clipboard-item-pinned? i))) items)
            (max 0 (- limit (count clipboard-item-pinned? items))))))))

(define/contract (make-clipboard-store [path (clipboard-path)]
                                       #:limit [limit 1000])
  (() (path? #:limit (and/c exact-integer? (>=/c 1) (<=/c 100000)))
   . ->* . clipboard-store?)
  (define store
    (clipboard-store path limit (load-items path limit) (make-semaphore 1)))
  (enforce-limit! store)
  store)

(define/contract (clipboard-record! store text)
  (-> clipboard-store? string? (or/c clipboard-item? #f))
  (if (non-empty-string? (string-trim text))
      (let ([capped
             (if (> (string-length text) max-item-chars)
                 (substring text 0 max-item-chars)
                 text)])
        (call-with-semaphore
         (clipboard-store-lock store)
         (lambda ()
           (define items (clipboard-store-items store))
           ;; Identical content moves to the front instead of duplicating.
           (define existing
             (findf (lambda (i) (string=? (clipboard-item-text i) capped)) items))
           (define item
             (if existing
                 (clipboard-item (clipboard-item-id existing)
                                 capped
                                 (current-inexact-milliseconds)
                                 (clipboard-item-pinned? existing)
                                 (sensitive-text? capped))
                 (clipboard-item (next-id!)
                                 capped
                                 (current-inexact-milliseconds)
                                 #f
                                 (sensitive-text? capped))))
           (set-clipboard-store-items!
            store
            (cons item
                  (filter (lambda (i) (not (string=? (clipboard-item-id i)
                                                     (clipboard-item-id item))))
                          items)))
           (enforce-limit! store)
           (persist! store)
           item)))
      #f))

(define/contract (clipboard-remove! store id)
  (-> clipboard-store? string? boolean?)
  (call-with-semaphore
   (clipboard-store-lock store)
   (lambda ()
     (define items (clipboard-store-items store))
     (set-clipboard-store-items!
      store
      (filter (lambda (i) (not (string=? (clipboard-item-id i) id))) items))
     (define removed? (< (length (clipboard-store-items store)) (length items)))
     (when removed? (persist! store))
     removed?)))

(define/contract (clipboard-toggle-pin! store id)
  (-> clipboard-store? string? (or/c clipboard-item? #f))
  (call-with-semaphore
   (clipboard-store-lock store)
   (lambda ()
     (define items (clipboard-store-items store))
     (define toggled
       (for/list ([item (in-list items)])
         (if (string=? (clipboard-item-id item) id)
             (struct-copy clipboard-item item
                          [pinned? (not (clipboard-item-pinned? item))])
             item)))
     (set-clipboard-store-items! store toggled)
     (define found
       (findf (lambda (i) (string=? (clipboard-item-id i) id)) toggled))
     (when found (persist! store))
     found)))

(define/contract (clipboard-clear! store)
  (-> clipboard-store? exact-integer?)
  (call-with-semaphore
   (clipboard-store-lock store)
   (lambda ()
     (define kept (filter clipboard-item-pinned? (clipboard-store-items store)))
     (define removed (- (length (clipboard-store-items store)) (length kept)))
     (set-clipboard-store-items! store kept)
     (persist! store)
     removed)))

(define/contract (clipboard-item-ref store id)
  (-> clipboard-store? string? (or/c clipboard-item? #f))
  (findf (lambda (i) (string=? (clipboard-item-id i) id))
         (clipboard-store-items store)))

(define/contract (clipboard-list store [n 50])
  (->* (clipboard-store?) (exact-nonnegative-integer?) (listof clipboard-item?))
  (define sorted
    (sort (clipboard-store-items store)
          >
          #:key (lambda (i) (if (clipboard-item-pinned? i) 1e18 (clipboard-item-ts i)))))
  (take sorted (min n (length sorted))))

(define/contract (clipboard-search store query)
  (-> clipboard-store? string? (listof clipboard-item?))
  (define needle (string-downcase (string-trim query)))
  (if (zero? (string-length needle))
      (clipboard-list store)
      (for/list ([item (in-list (clipboard-list store 100000))]
                 #:when (string-contains? (string-downcase (clipboard-item-text item)) needle))
        item)))

(define/contract (clipboard-count store)
  (-> clipboard-store? exact-nonnegative-integer?)
  (length (clipboard-store-items store)))
