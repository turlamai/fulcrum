#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
;; keeps every Racket-level consumer platform-safe: the kernel32 FFI runs
;; in a dedicated place (a blocking ReadDirectoryChangesW ties up the OS
;; thread of whichever place runs it - inside the main place that would
;; stall the whole scheduler), and on platforms without the watcher the
;; API returns #f - the catalog's once-per-process scan plus the OS
;; providers remain the only sources there, exactly as before this
;; module existed.
;;
;; Single-channel protocol: `pch` carries both directions - the
;; dispatcher sends 'stop orders on it and the place sends event batches
;; back. The dispatcher's reader thread ignores 'stop orders (they are
;; consumed by the place's non-blocking check between reads); the
;; marker wake in stop! guarantees the place gets a chance to consume
;; them between reads.
;;
;; Stop semantics: the place owns its directory handle exclusively, so
;; the stopper never touches a handle inside a pending synchronous read.
;; stop! sends 'stop over ctrl, then drops a marker file inside the
;; watched root to wake the blocked read; the place sees the stop order,
;; closes its own handle and exits.

(require racket/place
         racket/runtime-path
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

(struct fs-watch (pch ctrl reader stop-box stopped root))

(define (fs-watch-supported?)
  (eq? (system-type 'os) 'windows))

(define-runtime-path fs-watch-place-module "fs-watch-place.rkt")

(define (stop-marker-path root-str)
  (string-append root-str ".fulcrum-stop-"
                 (number->string (current-milliseconds) 36)))

(define (start-fs-watch! root on-events on-overflow)
  (cond
    [(not (fs-watch-supported?)) #f]
    [else
     (with-handlers ([exn:fail? (lambda (e)
                                  (eprintf "START-EXN: ~a\n" (exn-message e))
                                  #f)])
       (define root-str
         (let ([s (path->string (if (string? root) (string->path root) root))])
           ;; ReadDirectoryChangesW wants the trailing separator.
           (if (or (string-suffix? s "\\") (string-suffix? s "/"))
               s
               (string-append s "\\"))))
       (define-values (pch place-th)
         (dynamic-place fs-watch-place-module 'main))
       (define stopped (make-semaphore 0))
       (define stop-box (box #f))
       (define ctrl (box #f))
       (place-channel-put pch root-str)
       (define reader
         (thread
          (lambda ()
            (let loop ()
              (define msg (place-channel-get pch))
              (cond
                [(place-channel? msg)
                 (set-box! ctrl msg)
                 (loop)]
                [(eq? msg 'ready) (loop)]
                [(eq? msg 'open-failed) (semaphore-post stopped)]
                [(eq? msg 'overflow)
                 (with-handlers ([exn:fail? (lambda (_) (void))])
                   (on-overflow root-str))
                 (loop)]
                [else
                 (with-handlers ([exn:fail? (lambda (_) (void))])
                   (on-events msg))
                 (when (thread-dead? pch)
                   (semaphore-post stopped))
                 (loop)])))))
       (fs-watch pch ctrl pch reader stop-box stopped root-str))]))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-reader watch)))
    (set-box! (fs-watch-stop-box watch) #t)
    ;; The place is blocked in its read; each marker write wakes one
    ;; read, and the loop's non-blocking check consumes a 'stop order.
    ;; Two orders + two wakes cover the place sleeping through the
    ;; first wake (order or read scheduling on a loaded runner).
    (define (order-stop!)
      (place-channel-put (fs-watch-pch watch) 'stop)
      (with-handlers ([exn:fail? (lambda (_) (void))])
        (define marker (stop-marker-path (fs-watch-root watch)))
        (with-output-to-file marker #:exists 'truncate void)
        (with-handlers ([exn:fail? (lambda (_) (void))])
          (delete-file marker))))
    (order-stop!)
    (sync/timeout 3 (thread-dead-evt (fs-watch-reader watch)))
    (unless (thread-dead? (fs-watch-reader watch))
      (order-stop!)
      (sync/timeout 3 (thread-dead-evt (fs-watch-reader watch))))
    (void)))
