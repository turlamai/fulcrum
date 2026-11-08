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
;;   main -> place: (list root-string marker-path) once, at startup —
;;                  the only inbound message; post-startup inbound
;;                  delivery proved unreliable on Windows, so nothing
;;                  else is ever sent
;;   place -> main: 'ready once the handle is open, then lists of
;;                  changed path strings, or 'overflow, or 'open-failed
;;
;; Stop is a FILE, not a message: the main side creates the marker
;; file at the agreed path. Its change wakes the blocked read, and the
;; loop then stats the marker — its existence is the stop order. The
;; place closes its own handle — it is the handle's only user, so the
;; stopper never touches a handle that is inside a pending synchronous
;; read — and exits. If the wake failed (root vanished), the place
;; exits at the next real change (or process exit); the main side
;; joins bounded and cleans the marker up.

(require racket/place
         racket/string
         "fs-watch-ffi.rkt")

;; Marker paths carry ".fulcrum-stop-"; these wakeup artifacts are
;; ours, not user activity, and are filtered out of the event stream so
;; they never reach the coalescer. Keep this in sync with fs-watch.rkt.
(define (stop-marker? path)
  (string-contains? path ".fulcrum-stop-"))

(provide start-fs-watcher)

(define (start-fs-watcher ch)
  (define spec (place-channel-get ch))
  (define root (list-ref spec 0))
  (define marker-path (list-ref spec 1))
  (define handle (open-directory-watch! root))
  (cond
    [(not handle)
     (place-channel-put ch 'open-failed)
     (exit 0)]
    [else
     (place-channel-put ch 'ready)
     (let loop ([buffer (make-bytes 65536)])
       (define result
         (read-directory-changes! handle buffer
                                  (bytes-length buffer) root))
       (cond
         ;; The read failed (handle invalid): nothing left to watch.
         [(not result) (void)]
         ;; The marker's change woke this read; its existence is the
         ;; stop order. Close the handle we exclusively own and exit.
         [(file-exists? marker-path)
          (with-handlers ([exn:fail? (lambda (_) (void))])
            (close-directory-watch! handle))]
         [(eq? result 'overflow)
          (place-channel-put ch 'overflow)
          (loop buffer)]
         [else
          (define paths
            (filter (lambda (p) (not (stop-marker? p))) result))
          (place-channel-put ch paths)
          (loop buffer)]))])
  (exit 0))
