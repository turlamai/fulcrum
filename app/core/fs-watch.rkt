#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
;; keeps every Racket-level consumer platform-safe: on other platforms
;; the watch API returns #f — the catalog's once-per-process scan plus
;; the OS providers remain the only sources there, exactly as before
;; this module existed.
;;
;; Architecture: ReadDirectoryChangesW is bound blocking-synchronous
;; (no overlapped I/O), and a Racket place multiplexes all of its
;; threads on a single OS thread — a blocking foreign call inside the
;; main place would stall the whole scheduler (the process-wide hang
;; behind issue #190). The watch therefore runs in a dedicated place
;; (fs-watch-place.rkt's exported entry point) and a relay thread in
;; the main place forwards its messages to the caller's callbacks. The
;; kernel32 FFI surface is only ever loaded inside that place; this
;; module never touches it.
;;
;; Message protocol (see fs-watch-place.rkt): the place reports 'ready,
;; then lists of changed paths / 'overflow / 'open-failed; the main
;; side sends (list root marker-path) once at startup. The place
;; descriptor is synchronized directly (its sync result is the received
;; message); wrapping the channel in handle-evt loses messages, so it
;; is never done here. On stop! the relay does NOT wait for the place:
;; the place's runtime teardown can outlast any reasonable join, so the
;; relay watches a stop-box and exits on its own, while stop! joins the
;; place bounded (all the caller needs from it is the released
;; directory handle).
;;
;; Stop semantics: stop is a FILE, not a message — post-startup
;; inbound place-channel delivery proved unobservable for seconds on
;; the Windows CI/VM class of machines, so the agreed marker file
;; carries the order instead. stop! creates the marker; its change
;; wakes the blocked read; the place stats the marker (existence = the
;; stop order) and closes its own handle — Windows does not reliably
;; cancel a pending synchronous read, and a CloseHandle from another
;; thread while a read is pending deadlocks, so the stopper never
;; touches the handle. Overlapped I/O would remove the marker dance
;; and is the documented follow-up refinement.

(require racket/contract
         racket/place
         racket/runtime-path
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

(struct fs-watch (thread stop-box stopped root place))

(define (fs-watch-supported?)
  (eq? (system-type 'os) 'windows))

(define-runtime-path place-module "fs-watch-place.rkt")

;; The marker file lives inside the watched root: subtree watching makes
;; any change under root wake the blocked read. Milliseconds in base 16
;; keep concurrent watchers from clobbering each other's markers. The
;; place filters these paths out of the event stream (fs-watch-place.rkt).
(define (stop-marker-path root-str)
  (string-append root-str ".fulcrum-stop-"
                 (number->string (current-milliseconds) 16)))

(define (start-fs-watch! root on-events on-overflow)
  (cond
    [(not (fs-watch-supported?)) #f]
    [else
     (define root-str
       (let ([s (path->string (if (string? root) (string->path root) root))])
         ;; ReadDirectoryChangesW wants the trailing separator.
         (if (or (string-suffix? s "\\") (string-suffix? s "/"))
             s
             (string-append s "\\"))))
     ;; A marker left over by a watcher that died before its cleanup
     ;; would order the fresh place to stop immediately — clear it.
     (define marker-path (stop-marker-path root-str))
     (with-handlers ([exn:fail? (lambda (_) (void))])
       (when (file-exists? marker-path) (delete-file marker-path)))
     (define place
       (with-handlers ([exn:fail? (lambda (_) #f)])
         (dynamic-place place-module 'start-fs-watcher)))
     (cond
       [(not place) #f]
       [else
        ;; Startup spec is the only inbound message the place ever
        ;; expects: its watch target and the marker path that will
        ;; carry the stop order.
        (with-handlers ([exn:fail? (lambda (_) (void))])
          (place-channel-put place (list root-str marker-path)))
        (define stopped (make-semaphore 0))
        (define stop-box (box #f))
        (define ready (box #f)) ; 'ready | 'open-failed, set by the relay
        (define relay
          (thread
           (lambda ()
             (let loop ()
               ;; The place descriptor doubles as a receive event whose
               ;; sync result is the message; #f = the half-second tick,
               ;; which re-checks the stop-box below (the place's own
               ;; teardown can lag its handle release by a lot).
               (define msg
                 (sync/timeout 0.5
                   place
                   (handle-evt (place-dead-evt place)
                               (lambda (_) 'dead))))
               (cond
                 [(unbox stop-box) (void)]
                 [(eq? msg 'dead) (void)]
                 [(eq? msg #f) (loop)]
                 ;; 'ready keeps the relay alive: every later event batch
                 ;; flows through this same loop.
                 [(eq? msg 'ready) (set-box! ready 'ready) (loop)]
                 [(eq? msg 'open-failed) (set-box! ready 'open-failed)]
                 [(eq? msg 'overflow)
                  (with-handlers ([exn:fail? (lambda (_) (void))])
                    (on-overflow root-str))
                  (loop)]
                 ;; A list of changed paths (possibly empty after marker
                 ;; filtering): deliver and keep watching.
                 [else
                  (with-handlers ([exn:fail? (lambda (_) (void))])
                    (on-events msg))
                  (loop)]))
             (semaphore-post stopped))))
        (define watch (fs-watch relay stop-box stopped root-str place))
        ;; Bounded readiness: place boot is sub-second, but never hang
        ;; the caller — a timeout degrades to the no-watcher path.
        (let wait ([remaining 100]) ; 100 × 100 ms = 10 s
          (define state (unbox ready))
          (cond
            [(eq? state 'ready) watch]
            [(or (eq? state 'open-failed) (zero? remaining))
             (stop-fs-watch! watch)
             #f]
            [else (sleep 0.1) (wait (sub1 remaining))]))])]))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-thread watch)))
    ;; The relay exits within its next half-second tick; every watcher
    ;; thread contract (thread-dead? after stop) holds regardless of how
    ;; long the place's teardown takes.
    (set-box! (fs-watch-stop-box watch) #t)
    ;; The marker's change wakes the blocked read; the place stats it,
    ;; closes its own handle and exits. If the wake fails (root
    ;; vanished) the place exits at the next real change or process
    ;; exit; the join below never blocks forever.
    (define marker (stop-marker-path (fs-watch-root watch)))
    (with-handlers ([exn:fail? (lambda (_) (void))])
      (with-output-to-file marker #:exists 'truncate void))
    ;; Join the place bounded: the caller needs the directory handle
    ;; released (temp trees must be deletable after stop), nothing more.
    (sync/timeout 10 (place-dead-evt (fs-watch-place watch)))
    ;; Clean the artifact up (only meaningful once the place released
    ;; the directory; harmless otherwise — the next start clears stale
    ;; markers anyway).
    (with-handlers ([exn:fail? (lambda (_) (void))])
      (when (file-exists? marker) (delete-file marker))))
  (void))
