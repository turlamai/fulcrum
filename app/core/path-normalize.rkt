#lang racket/base

;; Shared file-path spelling rules for the file-search pipeline.
;;
;; Windows paths arrive in two spellings: the catalog walker stores native
;; backslashes, while Windows Search (SYSTEMINDEX) file: URLs decode to
;; forward slashes. Without one canonical spelling the same file occupies
;; two index rows, paints twice in the results, never matches reconcile's
;; backslash prefix scans, and explorer.exe cannot open the slash form.
;; POSIX paths keep their slashes: on macOS/Linux a backslash is a legal
;; filename character, never a separator.

(require racket/string)

(provide windows-path-form?
         normalize-file-path
         file-path-key)

;; A drive-absolute path (any letter, not a fixed C:/D: list) or a UNC
;; share in either separator spelling.
(define (windows-path-form? path)
  (or (regexp-match? #rx"^[A-Za-z]:" path)
      (string-prefix? path "//")
      (string-prefix? path "\\\\")))

(define (normalize-file-path path)
  (if (windows-path-form? path)
      (string-replace path "/" "\\")
      path))

;; Identity for deduplication: Windows paths are case-insensitive, POSIX
;; paths are not. Separators are already canonical after normalize-file-path.
(define (file-path-key path)
  (if (windows-path-form? path)
      (string-foldcase path)
      path))