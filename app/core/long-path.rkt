#lang racket/base

;; Expands Windows 8.3 short-name path components (RUNNER~1, FULCRU~2)
;; to their long form. Path forms must not leak into the file catalog:
;; rows are keyed by the string form of a path, so a watcher rooted at
;; a short-name path would upsert short-name rows while the scan wrote
;; long-name rows (or vice versa) — searches then miss, prunes never
;; match, and visibility flaps (issue #99 slice 1, Windows CI).
;;
;; Platform-safe: on non-Windows (and when kernel32 is unavailable) the
;; identity. The kernel32 binding is lazy so loading this module never
;; touches kernel32.

(require racket/promise
         ffi/unsafe
         "fs-watch-parse.rkt")

(provide long-path)

(define kernel32 (delay (with-handlers ([exn:fail? (lambda (_) #f)])
                          (ffi-lib "kernel32.dll"))))

(define GetLongPathNameW
  (delay
    (define k32 (force kernel32))
    (and k32
         ;; _string/utf-16 encodes input with a BOM (see
         ;; fs-watch-ffi.rkt) — kernel32 rejects it, so the same
         ;; BOM-less ctype is used here.
         (let ((in-bomless-utf16le
                (make-ctype _bytes
                            (lambda (st)
                              (bytes-append (string->utf16le-bytes st)
                                            (bytes 0 0)))
                            (lambda (b) b))))
           (get-ffi-obj
            "GetLongPathNameW" k32
            (_fun in-bomless-utf16le _bytes _uint32 -> _uint32))))))

;; Expands 8.3 short-name components to the long form. Input and output
;; are strings (or any path-ish, stringified). Best-effort: on any
;; failure the input returns unchanged.
(define (long-path p)
  (case (system-type 'os)
    ((windows)
     (define fn (force GetLongPathNameW))
     (if (not fn)
         p
         (let* ((s (if (string? p) p (path->string p)))
                (buf (make-bytes 2048))
                (n (fn s buf 1024))
                (long
                 (if (or (zero? n) (> n 1024))
                     s
                     (let decode ((i 0) (chars '()))
                       (define lo (bytes-ref buf i))
                       (define hi (bytes-ref buf (+ i 1)))
                       (define unit (bitwise-ior lo (arithmetic-shift hi 8)))
                       (cond
                         ((zero? unit) (list->string (reverse chars)))
                         (else (decode (+ i 2)
                                       (cons (integer->char unit)
                                             chars))))))))
           ;; Preserve the caller's type: paths stay paths.
           (if (string? p) long (string->path long)))))
    (else p)))
