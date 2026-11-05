#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
;; keeps every Racket-level consumer platform-safe: the kernel32 FFI
;; module is dynamic-required only on Windows (and a failed load degrades
;; to the no-op path), and on other platforms the watch API returns #f —
;; the catalog's once-per-process scan plus the OS providers remain the
;; only sources there, exactly as before this module existed.
;;
;; Stop semantics: ReadDirectoryChangesW blocks its OS thread, so stop!
;; closes the handle from the caller's thread; the blocked read returns
;; failure and the watch thread exits without touching the callback.

(require racket/contract
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

(struct fs-watch (thread handle stop-semaphore root))

(define (fs-watch-supported?)
  (eq? (system-type 'os) 'windows))

;; FFI bindings load lazily and exactly once; a load failure (non-Windows
;; host, stripped runtime) leaves this #f and every entry point no-ops.
(define ffi (box #f))

(define (load-ffi!)
  (when (not (unbox ffi))
    (define loaded
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (dynamic-require "fs-watch-ffi.rkt" #f)
        (define (get name type) (dynamic-require "fs-watch-ffi.rkt" name))
        (hash 'open (get 'open-directory-watch! '_fun)
              'read (get 'read-directory-changes! '_fun)
              'close (get 'close-directory-watch! '_fun)
              'open? (get 'watch-handle-open? '_fun))))
    (set-box! ffi loaded)))

(define (start-fs-watch! root on-events on-overflow)
  (cond
    [(not (fs-watch-supported?)) #f]
    [else
     (load-ffi!)
     (define f (unbox ffi))
     (cond
       [(not f) #f]
       [else
        (define root-str
          (let ([s (path->string (if (string? root) (string->path root) root))])
            ;; ReadDirectoryChangesW wants the trailing separator.
            (if (or (string-suffix? s "\\") (string-suffix? s "/"))
                s
                (string-append s "\\"))))
        (define handle ((hash-ref f 'open) root-str))
        (cond
          [(not (and handle ((hash-ref f 'open?) handle))) #f]
          [else
           (define stop (make-semaphore 0))
           (define buffer (make-bytes 65536))
           (define watch-thread
             (thread
              (lambda ()
                (let loop ()
                  (define result
                    ((hash-ref f 'read) handle buffer
                     (bytes-length buffer) root-str))
                  (cond
                    [(not result) (void)]
                    [(eq? result 'overflow)
                     (with-handlers ([exn:fail? (lambda (_) (void))])
                       (on-overflow root-str))
                     (loop)]
                    [else
                     (with-handlers ([exn:fail? (lambda (_) (void))])
                       (on-events result))
                     (loop)])))))
           (fs-watch watch-thread handle stop root-str)])])]))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-thread watch)))
    (semaphore-post (fs-watch-stop-semaphore watch))
    (define f (unbox ffi))
    (when f
      (with-handlers ([exn:fail? (lambda (_) (void))])
        ((hash-ref f 'close) (fs-watch-handle watch))))
    (void)))
