#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
<<<<<<< HEAD
;; keeps every Racket-level consumer platform-safe: the kernel32 FFI
;; module is dynamic-required only on Windows (and a failed load degrades
;; to the no-op path), and on other platforms the watch API returns #f —
;; the catalog's once-per-process scan plus the OS providers remain the
;; only sources there, exactly as before this module existed.
;;
;; Stop semantics: ReadDirectoryChangesW runs blocking-synchronous, and
;; Windows does not reliably cancel it — CloseHandle from another thread
;; while a read is pending deadlocks. The watch thread therefore owns
;; its handle exclusively: stop! sets a flag, then creates and deletes a
;; tiny marker file inside the watched root to wake the blocked read;
;; the thread sees the flag first, closes its own handle (safe — it is
;; the only user), and exits. Overlapped I/O would remove the
;; wake-marker dance and is the documented follow-up refinement.

(require racket/contract
=======
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
>>>>>>> feat/fs-live-watch-20261105
         racket/runtime-path
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

<<<<<<< HEAD
(struct fs-watch (thread stop-box stopped root))
=======
(struct fs-watch (pch place-thread reader stop-box stopped root))
>>>>>>> feat/fs-live-watch-20261105

(define (fs-watch-supported?)
  (eq? (system-type 'os) 'windows))

<<<<<<< HEAD
;; FFI bindings load lazily and exactly once; a load failure (non-Windows
;; host, stripped runtime) leaves this #f and every entry point no-ops.
(define ffi (box #f))

(define-runtime-path ffi-module "fs-watch-ffi.rkt")

(define (load-ffi!)
  (when (not (unbox ffi))
    (define loaded
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (dynamic-require ffi-module #f)
        (define (get name)
          (dynamic-require ffi-module name))
        (hash 'open (get 'open-directory-watch!)
              'read (get 'read-directory-changes!)
              'close (get 'close-directory-watch!)
              'open? (get 'watch-handle-open?))))
    (set-box! ffi loaded)))

;; The marker file lives inside the watched root: subtree watching makes
;; any change under root wake the blocked read. Milliseconds + a random
;; suffix keep concurrent watchers from clobbering each other's markers.
=======
(define-runtime-path fs-watch-place-module "fs-watch-place.rkt")

>>>>>>> feat/fs-live-watch-20261105
(define (stop-marker-path root-str)
  (string-append root-str ".fulcrum-stop-"
                 (number->string (current-milliseconds) 36)))

(define (start-fs-watch! root on-events on-overflow)
  (cond
    [(not (fs-watch-supported?)) #f]
    [else
<<<<<<< HEAD
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
           (define stop-box (box #f))
           (define stopped (make-semaphore 0))
           (define watch-thread
             (thread
              (lambda ()
                ;; The thread owns the handle: it closes it on every exit
                ;; path, so the stopper never touches a handle that is in
                ;; a pending synchronous read.
                (let loop ()
                  (cond
                    [(unbox stop-box) (void)]
                    [else
                     (define result
                       ((hash-ref f 'read) handle
                        (make-bytes 65536) 65536 root-str))
                     (cond
                       [(not result) (void)]
                       [(eq? result 'overflow)
                        (with-handlers ([exn:fail? (lambda (_) (void))])
                          (on-overflow root-str))
                        (loop)]
                       [else
                        (with-handlers ([exn:fail? (lambda (_) (void))])
                          (on-events result))
                        (loop)])]))
                ;; The thread owns the handle: close it here (its only
                ;; user), never from the stopper - a cross-thread
                ;; CloseHandle over a pending synchronous read deadlocks.
                (with-handlers ([exn:fail? (lambda (_) (void))])
                  ((hash-ref f 'close) handle))
                (semaphore-post stopped))))
           (fs-watch watch-thread stop-box stopped root-str)])])]))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-thread watch)))
    (set-box! (fs-watch-stop-box watch) #t)
    ;; Wake the blocked read: any change under the watched root returns
    ;; it, and the thread then exits through its own handle cleanup. A
    ;; cross-thread CloseHandle over a pending synchronous read would
    ;; deadlock, so the stopper never touches the handle.
    (with-handlers ([exn:fail? (lambda (_) (void))])
      (define marker (stop-marker-path (fs-watch-root watch)))
      (with-output-to-file marker #:exists 'truncate void)
      (with-handlers ([exn:fail? (lambda (_) (void))])
        (delete-file marker)))
    ;; Bounded join: the common case exits within milliseconds; if the
    ;; wake failed (root vanished) the thread exits at the next real
    ;; change without blocking this caller forever.
    (sync/timeout 5 (thread-dead-evt (fs-watch-thread watch)))
=======
     (with-handlers ([exn:fail? (lambda (e)
                                  (eprintf "fs-watch start failed: ~a\\n"
                                           (exn-message e))
                                  #f)])
       (define root-str
         (path->string
          (let ([p (if (string? root) (string->path root) root)])
            (if (or (string-suffix? (path->string p) "\\")
                    (string-suffix? (path->string p) "/"))
                p
                (string-append (path->string p) "\\")))))
       (define-values (pch _place-th)
         (dynamic-place fs-watch-place-module 'main))
       (define stopped (make-semaphore 0))
       (define stop-box (box #f))
       (define ctrl (box #f))
       (place-channel-put pch root-str)
       (define reader
         (thread
          (lambda ()
            (let reader-loop ()
              (define msg (place-channel-get pch))
              (cond
                [(place-channel? msg)
                 (set-box! ctrl msg)
                 (reader-loop)]
                [(eq? msg 'ready) (reader-loop)]
                [(eq? msg 'open-failed) (semaphore-post stopped)]
                [(eq? msg 'overflow)
                 (with-handlers ([exn:fail? (lambda (_) (void))])
                   (on-overflow root-str))
                 (reader-loop)]
                [else
                 (with-handlers ([exn:fail? (lambda (_) (void))])
                   (on-events msg))
                 (when (thread-dead? pch)
                   (semaphore-post stopped))
                 (reader-loop)])))))
       (fs-watch pch pch reader stop-box stopped root-str))]))

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
>>>>>>> feat/fs-live-watch-20261105
    (void)))
