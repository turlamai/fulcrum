#lang racket/base

;; Platform-independent unit tests for the live watcher's pure
;; FILE_NOTIFY_INFORMATION parsing (app/core/fs-watch-parse.rkt). The
;; end-to-end watcher test (tests/fs-watch-test.rkt) can only exercise
;; this logic on Windows CI; these run everywhere, which is exactly the
;; insurance the parse path's history calls for: its first real-event
;; run crashed on the first character of any real filename (issue #190).

(require rackunit
         "../app/core/fs-watch-parse.rkt")

;; Encodes a string as UTF-16LE code units (surrogate pairs included),
;; matching what the kernel writes into a FILE_NOTIFY_INFORMATION name.
(define (utf16le-bytes s)
  (apply
   bytes
   (apply append
          (for/list ([ch (in-string s)])
            (define cp (char->integer ch))
            (if (<= cp #xFFFF)
                (list (bitwise-and cp #xFF) (arithmetic-shift cp -8))
                (let ([v (- cp #x10000)])
                  (define high (+ #xD800 (arithmetic-shift v -10)))
                  (define low (+ #xDC00 (bitwise-and v #x3FF)))
                  (append (list (bitwise-and high #xFF) (arithmetic-shift high -8))
                          (list (bitwise-and low #xFF) (arithmetic-shift low -8)))))))))

;; One FILE_NOTIFY_INFORMATION record: next | action | name-len | name.
;; FileNameLength counts UTF-16 code-unit BYTES (an astral character is
;; four bytes), exactly what the kernel writes — not string-length.
(define (record name [action 1] [next 0])
  (define name-bytes (utf16le-bytes name))
  (bytes-append (le-bytes next)
                (le-bytes action)
                (le-bytes (bytes-length name-bytes))
                name-bytes))

(define (le-bytes n)
  (bytes (bitwise-and n #xFF)
         (bitwise-and (arithmetic-shift n -8) #xFF)
         (bitwise-and (arithmetic-shift n -16) #xFF)
         (bitwise-and (arithmetic-shift n -24) #xFF)))

(test-case "a plain ASCII notification parses (the issue #190 regression)"
  (define buf (record "wakeup.txt"))
  (define-values (paths overflow?) (parse-changes! buf (bytes-length buf) "C:\\root\\"))
  (check-false overflow?)
  (check-equal? paths (list "C:\\root\\wakeup.txt")))

(test-case "CJK filenames survive UTF-16LE decoding"
  (define buf (record "会议纪要.txt"))
  (define-values (paths _) (parse-changes! buf (bytes-length buf) "C:\\root\\"))
  (check-equal? paths (list "C:\\root\\会议纪要.txt")))

(test-case "astral-plane filenames recombine surrogate pairs"
  (define buf (record "doc-🦆.md"))
  (define-values (paths _) (parse-changes! buf (bytes-length buf) "C:\\root\\"))
  (check-equal? paths (list "C:\\root\\doc-🦆.md")))

(test-case "multi-record buffers follow the next-entry chain"
  (define first (record "alpha.txt" 1 (bytes-length (record "alpha.txt" 1 0))))
  (define second (record "beta.txt"))
  (define buf (bytes-append first second))
  (define-values (paths _) (parse-changes! buf (bytes-length buf) "C:\\root\\"))
  ;; Records with next=0 terminate the walk; order is unspecified, so
  ;; compare as a set.
  (check-equal? (length paths) 2)
  (check-equal? (sort paths string<?)
                (list "C:\\root\\alpha.txt" "C:\\root\\beta.txt")))

(test-case "records beyond byte-count are not read"
  (define buf (bytes-append (record "kept.txt")
                            (record "outside.txt")))
  ;; Only the first record's span is inside byte-count.
  (define-values (paths _) (parse-changes! buf (bytes-length (record "kept.txt")) "C:\\root\\"))
  (check-equal? paths (list "C:\\root\\kept.txt")))

(test-case "the encoder round-trips through the decoder, BMP and astral"
  (for ([s (in-list '("wakeup.txt" "会议纪要.txt" "doc-🦆.md" ""))])
    (define encoded (string->utf16le-bytes s))
    (check-equal? (utf16le-region->string encoded 0 (bytes-length encoded))
                  s)))

(test-case "encoder produces exact UTF-16LE bytes for an astral name"
  ;; 🦆 U+1F986 → D83E DD86 (little-endian: 3E D8 86 DD).
  (check-equal? (string->utf16le-bytes "🦆")
                (bytes #x3E #xD8 #x86 #xDD)))
