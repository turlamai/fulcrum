#lang racket/base

;; Live filesystem watching (issue #99 slice 1: Windows). The dispatcher
;; keeps every Racket-level consumer platform-safe: on other platforms
;; the watch API returns #f — the catalog's once-per-process scan plus
;; the OS providers remain the only sources there, exactly as before
;; this module existed.
;;
;; Architecture (issue #190 follow-up): ReadDirectoryChangesW runs with
;; OVERLAPPED I/O — the read is issued asynchronously from a plain watch
;; thread and reaped with bounded WaitForSingleObject calls, so no
;; foreign call ever blocks long enough to stall the place's scheduler.
;; (The blocking-synchronous binding froze the whole process from a
;; plain thread; the dedicated-place rebuild could not ship because the
;; packaged app's embedded runtime spins forever inside dynamic-place.)
;;
;; The kernel32 FFI surface is loaded lazily and only ever on Windows;
;; this module never touches it at load time.
;;
;; Stop semantics: stop! sets the stop-box and the thread calls
;; CancelIoEx — the documented safe cancellation for overlapped reads.
;; The pending read completes with an abort status, the thread sees the
;; box, closes its own handle (the only user — a cross-thread
;; CloseHandle over a pending read deadlocks), and exits; stop! joins
;; bounded.

(require racket/contract
         racket/runtime-path
         racket/string)

(provide fs-watch-supported?
         (struct-out fs-watch)
         start-fs-watch!
         stop-fs-watch!)

(struct fs-watch (thread stop-box stopped handle root))

(define (fs-watch-supported?)
  (eq? (system-type 'os) 'windows))

(define-runtime-path ffi-module "fs-watch-ffi.rkt")
;; Declared so the packager ships it alongside the ffi module (its
;; sibling require); fs-watch.rkt itself only loads the ffi module.
(define-runtime-path parse-module "fs-watch-parse.rkt")

;; FFI bindings load lazily and exactly once; a load failure (non-
;; Windows host, stripped runtime) leaves this #f and every entry point
;; no-ops.
(define ffi (box #f))

(define (load-ffi!)
  (when (not (unbox ffi))
    (define loaded
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (dynamic-require ffi-module #f)
        (define (get name)
          (dynamic-require ffi-module name))
        (hash 'open (get 'open-directory-watch!)
              'open? (get 'watch-handle-open?)
              'close (get 'close-directory-watch!)
              'event (get 'make-wait-event!)
              'overlapped (get 'make-overlapped!)
              'issue (get 'issue-read!)
              'wait (get 'wait-read!)
              'collect (get 'collect-read!)
              'cancel (get 'cancel-read!))))
    (set-box! ffi loaded)))

;; One wait-and-reap iteration of the watch loop. Returns two values:
;; 'continue | 'stop, and whether a read is now pending. Every foreign
;; call here is bounded (the wait at most 100 ms), so the Racket
;; scheduler stays live throughout.
(define (watch-poll! f handle event ov buffer root-str stop-box pending?
                     on-events on-overflow)
  (cond
    ((unbox stop-box)
     ;; Cancel the pending read so the wait below reaps an abort
     ;; instead of spinning until the next real change.
     (when pending? ((hash-ref f 'cancel) handle))
     ((hash-ref f 'wait) event 500)
     (values 'stop pending?))
    (else
     (define state ((hash-ref f 'wait) event 100))
     (case state
       ((timeout) (values 'continue pending?))
       ((failed) (values 'exit pending?))
       ((done)
        (define result ((hash-ref f 'collect) handle buffer root-str ov))
        (cond
          ;; Aborted or failed: nothing left to reap.
          ((not result) (values 'exit pending?))
          ;; Kernel buffer overflow: slice 1 notes the miss honestly;
          ;; the next full scan (next app start) heals it. #99 tracks
          ;; the rescan fast-path.
          ((eq? result 'overflow)
           (with-handlers ((exn:fail? (lambda (_) (void))))
             (on-overflow root-str))
           (values 'continue
                   ((hash-ref f 'issue) handle buffer
                    (bytes-length buffer) event ov)))
          (else
           (with-handlers ((exn:fail? (lambda (_) (void))))
             (on-events result))
           (values 'continue
                   ((hash-ref f 'issue) handle buffer
                    (bytes-length buffer) event ov)))))))))

;; The watch thread body: own the handle, loop until stopped, then
;; close the handle (its only user — a cross-thread CloseHandle over a
;; pending read deadlocks; CancelIoEx is the safe stop lever) and post.
(define (watch-thread-body! f handle event ov root-str stop-box
                            stopped on-events on-overflow)
  (define buffer (make-bytes 65536))
  (let loop ((pending? ((hash-ref f 'issue) handle buffer
                        (bytes-length buffer) event ov)))
    (define-values (verdict now-pending?)
      (watch-poll! f handle event ov buffer root-str stop-box pending?
                   on-events on-overflow))
    (when (eq? verdict 'continue)
      (loop now-pending?)))
  (with-handlers ((exn:fail? (lambda (_) (void))))
    ((hash-ref f 'close) handle))
  (semaphore-post stopped))

(define (start-fs-watch! root on-events on-overflow)
  (cond
    ((not (fs-watch-supported?)) #f)
    (else
     (load-ffi!)
     (define f (unbox ffi))
     (cond
       ((not f) #f)
       (else
        (define root-str
          (let ((s (path->string (if (string? root) (string->path root) root))))
            ;; ReadDirectoryChangesW wants the trailing separator.
            (if (or (string-suffix? s "\\") (string-suffix? s "/"))
                s
                (string-append s "\\"))))
        (define handle ((hash-ref f 'open) root-str))
        (cond
          ((not (and handle ((hash-ref f 'open?) handle))) #f)
          (else
           (define event ((hash-ref f 'event)))
           (define ov (and event ((hash-ref f 'overlapped) event)))
           (cond
             ((not (and event ov)) #f)
             (else
              (define stop-box (box #f))
              (define stopped (make-semaphore 0))
              (define watch-thread
                (thread
                 (lambda ()
                   (watch-thread-body! f handle event ov root-str stop-box
                                       stopped on-events on-overflow))))
              (fs-watch watch-thread stop-box stopped handle root-str))))))))))

(define (stop-fs-watch! watch)
  (unless (or (not watch) (thread-dead? (fs-watch-thread watch)))
    ;; Order the stop; the thread cancels its own pending read and
    ;; exits within one bounded tick.
    (set-box! (fs-watch-stop-box watch) #t)
    ;; Bounded join: CancelIoEx makes the abort deterministic; if the
    ;; wake fails anyway the thread exits at the next real change
    ;; without blocking this caller forever.
    (sync/timeout 5 (thread-dead-evt (fs-watch-thread watch))))
  (void))
