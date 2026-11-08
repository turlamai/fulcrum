#lang racket/base

;; Pure FILE_NOTIFY_INFORMATION buffer parsing for the live file watcher
;; (issue #99 slice 1). Split from fs-watch-ffi.rkt so the logic is
;; loadable — and unit-testable — on every platform: it has no kernel32
;; dependency, and the platform-independent test suite exercises it on
;; Linux, macOS and Windows CI alike. The Windows-only FFI module uses
;; it to turn raw ReadDirectoryChangesW buffers into changed paths.
;;
;; This code's history earns its tests: its first real-event run (the
;; first character of any real filename) crashed on `(>= #f #xDC00)` —
;; the surrogate-pair probe compared the #f that the `(and ...)` chain
;; produces for every non-surrogate unit (issue #190) — and it had never
;; once executed successfully before that, because the watcher above it
;; was dead in two different ways (issue #189's merge, then #190's
;; main-place freeze).

(require racket/string)

(provide le32
         utf16le-region->string
         parse-changes!
         string->utf16le-bytes)

;; UTF-16LE code units with surrogate pairs, no BOM — the same encoding
;; the kernel writes into notification names, used by the FFI side when
;; handing CreateFileW a watched path. Surrogate arithmetic here earned
;; a test the hard way: `v 10` (a left shift) where `v -10` was meant
;; produced garbage high surrogates for every astral-plane character.
(define (string->utf16le-bytes s)
  (apply
   bytes
   (apply append
          (for/list ([ch (in-string s)])
            (define cp (char->integer ch))
            (cond
              [(<= cp #xFFFF)
               (list (bitwise-and cp #xFF) (arithmetic-shift cp -8))]
              [else
               (define v (- cp #x10000))
               (define high (+ #xD800 (arithmetic-shift v -10)))
               (define low (+ #xDC00 (bitwise-and v #x3FF)))
               (list (bitwise-and high #xFF) (arithmetic-shift high -8)
                     (bitwise-and low #xFF) (arithmetic-shift low -8))])))))

;; Little-endian uint32 from buffer at byte offset (FILE_NOTIFY_INFORMATION
;; fields are LE uint32s; reading them bytewise avoids any import question).
(define (le32 buffer offset)
  (bitwise-ior (bytes-ref buffer offset)
               (arithmetic-shift (bytes-ref buffer (+ offset 1)) 8)
               (arithmetic-shift (bytes-ref buffer (+ offset 2)) 16)
               (arithmetic-shift (bytes-ref buffer (+ offset 3)) 24)))

;; UTF-16LE code units from a FILE_NOTIFY_INFORMATION buffer, surrogate
;; pairs combined so astral-plane filenames survive. `low` is #f unless
;; the unit is a high surrogate AND two more bytes exist, so the low
;; surrogate range test must expect #f — every BMP character lands here.
(define (utf16le-region->string buffer start end)
  (let loop ([i start] [chars '()])
    (cond
      [(>= i end) (list->string (reverse chars))]
      [else
       (define unit (bitwise-ior (bytes-ref buffer i)
                                 (arithmetic-shift (bytes-ref buffer (+ i 1)) 8)))
       (define low
         (and (>= unit #xD800) (<= unit #xDBFF) (<= (+ i 4) end)
              (bitwise-ior (bytes-ref buffer (+ i 2))
                           (arithmetic-shift (bytes-ref buffer (+ i 3)) 8))))
       (cond
         [(and low (>= low #xDC00) (<= low #xDFFF))
          (define cp (+ #x10000
                        (arithmetic-shift (- unit #xD800) 10)
                        (- low #xDC00)))
          (loop (+ i 4) (cons (integer->char cp) chars))]
         [else
          (loop (+ i 2) (cons (integer->char unit) chars))])])))

;; Walks one notification buffer. Returns (values changed-paths overflow?)
;; where changed-paths are native full paths under root, in unspecified
;; order, possibly duplicated (renames contribute both halves). The root
;; arrives with its trailing separator from the dispatcher; do not add
;; a second one.
(define (parse-changes! buffer byte-count root)
  (define sep-string
    (if (or (string-suffix? root "\\") (string-suffix? root "/"))
        ""
        "\\"))
  (let loop ([offset 0] [paths '()])
    (if (>= offset byte-count)
        (values (reverse paths) #f)
        (let* ([next (le32 buffer offset)]
               [name-len (le32 buffer (+ offset 8))]
               [name (utf16le-region->string buffer
                                             (+ offset 12)
                                             (+ offset 12 name-len))]
               [full (string-append root sep-string name)])
          (cond
            [(zero? next) (values (cons full (reverse paths)) #f)]
            [else (loop (+ offset next) (cons full paths))])))))
