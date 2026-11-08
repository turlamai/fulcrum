#lang racket/base

;; The place half of the Windows live watcher (issue #99). A blocking
;; ReadDirectoryChangesW call ties up the OS thread of whichever Racket
;; thread runs it — inside the main place that would stall the whole
;; scheduler (all of a place's Racket threads share one OS thread), so
;; the watcher runs in this dedicated place and reports events over the
;; place channel. All kernel32 bindings and the notification-buffer
;; parsing are single-sourced from fs-watch-ffi.rkt; this module is
;; protocol only and is loaded only inside the place.
;;
;; Entry point (the name dynamic-place is given by fs-watch.rkt):
;; `start-fs-watcher` receives the peer channel of the place descriptor.
;;
;; Protocol (over `ch`):
;;   main -> place: the root string to watch, then 'stop at any time
;;   place -> main: 'ready once the handle is open, then lists of
;;                  changed path strings, or 'overflow, or 'open-failed
;;
;; Stop: the main side sends 'stop and drops a marker file inside the
;; watched root (fs-watch.rkt's stop-marker-path). The marker's change
;; wakes the blocked read; the loop then sees the stop order, closes its
;; own handle — it is the handle's only user, so the stopper never
;; touches a handle that is inside a pending synchronous read — and
;; exits. If the root vanished, the wake fails and the place exits at
;; the next real change (or process exit); the main side joins bounded.

(require racket/place
         racket/string
         "fs-watch-ffi.rkt")

;; The dispatcher's stop-marker-path writes root + ".fulcrum-stop-" +
;; base-36 milliseconds; these wakeup artifacts are ours, not user
;; activity, and are filtered out of the event stream so they never
;; reach the coalescer. Keep this in sync with fs-watch.rkt.
(define (stop-marker? path)
  (regexp-match? #px"\\\\\\.fulcrum-stop-" path))

(provide start-fs-watcher)

(define (start-fs-watcher ch)
  (define root (place-channel-get ch))
  ;; A 'stop racing in before the root means the watcher was never
  ;; really armed: nothing to watch, nothing to clean up.
  (when (eq? root 'stop) (exit 0))
  (define handle (open-directory-watch! root))
  (cond
    [(not handle)
     (place-channel-put ch 'open-failed)
     (exit 0)]
    [else
     (place-channel-put ch 'ready)
     (let loop ([buffer (make-bytes 65536)])
       (cond
         ;; Stop is polled between reads; the marker wake guarantees the
         ;; blocked read returns, so the poll is always reached.
         [(equal? 'stop (sync/timeout 0 ch))
          (with-handlers ([exn:fail? (lambda (_) (void))])
            (close-directory-watch! handle))]
         [else
          (define result
            (read-directory-changes! handle buffer
                                     (bytes-length buffer) root))
          (cond
            ;; The read failed (handle invalid): nothing left to watch.
            [(not result) (void)]
            [(eq? result 'overflow)
             (place-channel-put ch 'overflow)
             (loop buffer)]
            [else
             (define paths
               (filter (lambda (p) (not (stop-marker? p))) result))
             (place-channel-put ch paths)
             (loop buffer)])]))])
  (exit 0))
