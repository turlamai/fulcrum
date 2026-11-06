#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
;; keeps every Racket-level consumer platform-safe: the kernel32 FFI runs
;; in a dedicated place (a blocking ReadDirectoryChangesW ties up the OS
;; thread of whichever place runs it - inside the main place that would
;; stall the whole scheduler; confirmed live on the Windows VM), and on
;; platforms without the watcher the API returns #f - the catalog's
;; once-per-process scan plus the OS providers remain the only sources
;; there, exactly as before this module existed.
;;
;; Stop semantics: the place owns its directory handle exclusively, so
;; the stopper never touches a handle inside a pending synchronous read.
;; stop! sends 'stop over the channel, then drops a marker file inside
;; the watched root to wake the blocked read; the place sees the stop
;; order, closes its own handle and exits. Overlapped I/O would remove
;; the marker dance and is the documented follow-up refinement.

(require racket/place
         racket/runtime-path
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

;; The reader thread translates place messages into the caller's
;; callbacks; place-th is the kernel32-side place's descriptor thread.
(struct fs-watch (pch place-thread reader stop-box stopped root))

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
     (with-handlers ([exn:fail? (lambda (_) #f)])
       (define root-str
         (let ([s (path->string (if (string? root) (string->path root) root))])
           ;; ReadDirectoryChangesW wants the trailing separator.
           (if (or (string-suffix? s "\\") (string-suffix? s "/"))
               s
               (string-append s "\\"))))
       ;; dynamic-place returns ONE value: the place descriptor (whose
       ;; descriptor thread is place-th below - a place descriptor IS a
       ;; thread-like object for sync/deadline purposes).
       (define pch
         (dynamic-place fs-watch-place-module 'main))
       (define stopped (make-semaphore 0))
       (define stop-box (box #f))
       (place-channel-put pch root-str)
       (define reader
         (thread
          (lambda ()
            (let loop ()
              (define msg (place-channel-get pch))
              (cond
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
       (fs-watch pch pch reader stop-box stopped root-str))]))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-reader watch)))
    (set-box! (fs-watch-stop-box watch) #t)
    (place-channel-put (fs-watch-pch watch) 'stop)
    ;; Wake the blocked read: any change under the watched root returns
    ;; it, and the place then exits through its own handle cleanup.
    (with-handlers ([exn:fail? (lambda (_) (void))])
      (define marker (stop-marker-path (fs-watch-root watch)))
      (with-output-to-file marker #:exists 'truncate void)
      (with-handlers ([exn:fail? (lambda (_) (void))])
        (delete-file marker)))
    ;; Bounded joins: the common case exits within milliseconds; if the
    ;; wake failed (root vanished) the place exits at the next real
    ;; change without blocking this caller forever - and the place is a
    ;; separate OS process, so a straggler cannot hold the main app.
    (sync/timeout 5 (thread-dead-evt (fs-watch-reader watch)))
    (sync/timeout 2 (fs-watch-place-thread watch))
    (semaphore-post (fs-watch-stopped watch))
    (void)))
