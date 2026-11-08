#lang racket/base

;; Windows ReadDirectoryChangesW bindings for the live file watcher.
;; Loaded via dynamic-require only on Windows — kernel32.dll does not
;; exist elsewhere.
;;
;; Overlapped I/O (issue #190 follow-up): the read is issued
;; asynchronously and reaped with bounded WaitForSingleObject calls, so
;; every foreign call stays short and the calling Racket thread never
;; freezes the place's scheduler — no dedicated place is needed at all
;; (the packaged app's embedded runtime spins forever inside
;; dynamic-place, so the place architecture cannot ship). Stop is
;; CancelIoEx, the documented safe cancellation for overlapped reads;
;; the handle is still owned exclusively by the watch thread.
;;
;; Buffer parsing (pure logic, no kernel32 dependency) lives in
;; fs-watch-parse.rkt so the platform-independent test suite exercises
;; it on every OS.

(require ffi/unsafe
         racket/string
         "fs-watch-parse.rkt")


(provide open-directory-watch!
         close-directory-watch!
         watch-handle-open?
         make-wait-event!
         make-overlapped!
         issue-read!
         wait-read!
         collect-read!
         cancel-read!)

(define kernel32 (ffi-lib "kernel32.dll"))

;; HANDLE is an opaque 64-bit integer on Windows. Declaring it as
;; _HANDLE* (pointer-to-HANDLE) made the FFI dereference CreateFileW's
;; return value as a memory address - INVALID_HANDLE_VALUE (-1) then
;; crashed the call and every watcher start degraded to #f.
(define _HANDLE _int64)

;; _string/utf-16 encodes foreign input with a byte-order mark; Windows
;; would read the BOM as part of the path and fail the open. This ctype
;; encodes BOM-less UTF-16LE (fs-watch-parse.rkt, tested on every
;; platform) with a two-byte null terminator instead.
(define _bomless-utf16le
  (make-ctype _bytes
              (lambda (s)
                (bytes-append (string->utf16le-bytes s) (bytes 0 0)))
              (lambda (b) b)))

(define CreateFileW
  (get-ffi-obj
   "CreateFileW" kernel32
   (_fun _bomless-utf16le        ; lpFileName (see below)
         _uint32                 ; dwDesiredAccess (FILE_LIST_DIRECTORY)
         _uint32                 ; dwShareMode
         _pointer                ; lpSecurityAttributes
         _uint32                 ; dwCreationDisposition
         _uint32                 ; dwFlagsAndAttributes
         _HANDLE                 ; hTemplateFile (NULL = 0)
         -> _HANDLE)))

(define CloseHandle
  (get-ffi-obj "CloseHandle" kernel32 (_fun _HANDLE -> _int32)))

(define CreateEventW
  (get-ffi-obj "CreateEventW" kernel32
   (_fun _pointer                 ; lpEventAttributes (NULL)
         _int32                   ; bManualReset (TRUE)
         _int32                   ; bInitialState (FALSE: unsignaled)
         _pointer                 ; lpName (NULL: unnamed)
         -> _HANDLE)))

(define WaitForSingleObject
  (get-ffi-obj "WaitForSingleObject" kernel32
   (_fun _HANDLE _uint32 -> _uint32)))

(define GetOverlappedResult
  (get-ffi-obj "GetOverlappedResult" kernel32
   (_fun _HANDLE                  ; hFile
         _pointer                 ; lpOverlapped
         _pointer                 ; lpNumberOfBytesTransferred
         _int32                   ; bWait (FALSE: only completed results)
         -> _int32)))

(define CancelIoEx
  (get-ffi-obj "CancelIoEx" kernel32
   (_fun _HANDLE _pointer -> _int32)))

(define ReadDirectoryChangesW
  (get-ffi-obj
   "ReadDirectoryChangesW" kernel32
   (_fun _HANDLE                 ; hDirectory
         _pointer                ; lpBuffer
         _uint32                 ; nBufferLength
         _int32                  ; bWatchSubtree
         _uint32                 ; dwNotifyFilter
         _pointer                ; lpBytesReturned (ignored for overlapped)
         _pointer                ; lpOverlapped
         _pointer                ; lpCompletionRoutine (NULL)
         -> _int32)))

(define FILE_LIST_DIRECTORY          #x0001)
(define OPEN_EXISTING                3)
(define FILE_SHARE_READ              #x1)
(define FILE_SHARE_WRITE             #x2)
(define FILE_SHARE_DELETE            #x4)
(define FILE_FLAG_BACKUP_SEMANTICS   #x02000000)
(define FILE_FLAG_OVERLAPPED         #x40000000)
(define FILE_NOTIFY_CHANGE_FILE_NAME  #x1)
(define FILE_NOTIFY_CHANGE_DIR_NAME   #x2)
(define FILE_NOTIFY_CHANGE_LAST_WRITE #x10)

(define WAIT_OBJECT_0 0)
(define WAIT_TIMEOUT  258)

(define INVALID_HANDLE_VALUE-LOW -1)

(define (open-directory-watch! directory)
  (define handle
    (CreateFileW directory
                 FILE_LIST_DIRECTORY
                 (+ FILE_SHARE_READ FILE_SHARE_WRITE FILE_SHARE_DELETE)
                 #f
                 OPEN_EXISTING
                 (+ FILE_FLAG_BACKUP_SEMANTICS FILE_FLAG_OVERLAPPED)
                 0))
  (if (= handle INVALID_HANDLE_VALUE-LOW) #f handle))

(define (watch-handle-open? handle)
  (> handle 0))

(define (close-directory-watch! handle)
  (not (zero? (CloseHandle handle))))

;; An auto-reset event the overlapped completion signals. The watcher
;; allocates one per watch; never signaled until a read completes.
(define (make-wait-event!)
  (define event (CreateEventW #f 1 0 #f))
  (if (= event INVALID_HANDLE_VALUE-LOW) #f event))

;; The OVERLAPPED state block: Internal, InternalHigh, Offset,
;; OffsetHigh, hEvent. Raw (non-moving, non-GC) memory — the kernel
;; writes into it after the issuing call returns. Zeroed once; the
;; Offset fields are ignored for directory handles.
(define (make-overlapped! event)
  (define ov (malloc _bytes 64 'raw))
  (memcpy ov (make-bytes 64) 64)
    ;; x64 OVERLAPPED: Internal(0) InternalHigh(1) Offset(2) OffsetHigh(3)
  ;; hEvent(4) in 8-byte slots - hEvent at byte offset 24.
  (ptr-set! ov _int64 4 event)
  ov)

;; Issues one overlapped directory read: returns immediately with the
;; read pending; its completion (or cancellation) signals the event.
;; Reads are serialized per watcher, so one OVERLAPPED block (created
;; once by the watcher, bound to its event) serves every issue.
(define (issue-read! handle buffer buffer-size event ov)
  (define ok
    (ReadDirectoryChangesW handle
                           buffer buffer-size
                           1   ; bWatchSubtree: the whole root
                           (+ FILE_NOTIFY_CHANGE_FILE_NAME
                              FILE_NOTIFY_CHANGE_DIR_NAME
                              FILE_NOTIFY_CHANGE_LAST_WRITE)
                           (malloc _uint32 'raw)
                           ov
                           #f))
  (not (zero? ok)))

;; Bounded wait on the pending read's completion event. Returns
;; 'done (the read completed or was cancelled — collect-read! decides),
;; 'timeout (still pending — return control to the scheduler), or
;; 'failed (the wait itself failed; treat as terminal).
(define (wait-read! event timeout-ms)
  (case (WaitForSingleObject event timeout-ms)
    [(0) 'done]
    [(258) 'timeout]
    [else 'failed]))

;; Reaps the completed read. Returns a list of changed native paths
;; (marker paths filtered by the caller), 'overflow when the kernel had
;; to drop events (the caller must resynchronize the root), or #f when
;; the read failed — which after a CancelIoEx stop simply means exit.
(define (collect-read! handle buffer root ov)
  (define bytes-returned (malloc _uint32 'raw))
  (define ok (GetOverlappedResult handle ov bytes-returned 0))
  (cond
    [(zero? ok) #f]
    [else
     (define n (ptr-ref bytes-returned _uint32))
     (cond
       [(zero? n) 'overflow]
       [else
        (define-values (paths overflow?)
          (parse-changes! buffer n root))
        (if overflow? 'overflow paths)])]))

;; Cancels every pending read on the handle. Overlapped reads are the
;; documented safe cancellation target — the pending read completes
;; with an abort status instead of wedging the OS thread (the
;; synchronous pattern deadlocked on cross-thread CloseHandle).
(define (cancel-read! handle)
  (not (zero? (CancelIoEx handle #f))))
