#lang racket/base

;; The place half of the Windows live watcher (issue #99). A blocking
;; ReadDirectoryChangesW call ties up the OS thread of whichever Racket
<<<<<<< HEAD
;; thread runs it — inside the main place that would stall the whole
;; scheduler — so the watcher runs in this dedicated place and reports
;; events over the place channel.
;;
;; Protocol (over `ch`, created by dynamic-place):
;;   main -> place: (list root-string) to start, then 'stop at any time
=======
;; thread runs it - inside the main place that would stall the whole
;; scheduler - so the watcher runs in this dedicated place and reports
;; events over the place channel.
;;
;; Protocol (over `ch`, created by dynamic-place):
;;   main -> place: root-string to start, then 'stop at any time
>>>>>>> feat/fs-live-watch-20261105
;;   place -> main: 'ready once the handle is open, then batches of
;;                  changed path strings, or 'overflow, or 'open-failed
;;
;; Stop: the main process sends 'stop and drops a marker file inside the
;; watched root. The marker's change wakes the blocked read; the loop
;; then sees the stop order, closes its own handle and exits. The
;; handle is owned exclusively by this place, so the stopper never
;; touches a handle that is inside a pending synchronous read.

(require ffi/unsafe
         racket/file
         racket/place
         racket/string)

(define kernel32 (ffi-lib "kernel32.dll"))

(define _HANDLE _int64)

;; _string/utf-16 encodes foreign input with a byte-order mark; Windows
<<<<<<< HEAD
;; would read the BOM as part of the path and fail the open. This ctype
;; encodes BOM-less UTF-16LE with a two-byte null terminator instead
;; (hand-written encoder with surrogate pairs).
=======
;; would read the BOM as part of the path and fail the open. Encode
;; BOM-less UTF-16LE with a two-byte null terminator instead (surrogate
;; pairs included).
>>>>>>> feat/fs-live-watch-20261105
(define (string->utf16le-bytes s)
  (define units
    (for/list ([chr (in-string s)])
      (define cp (char->integer chr))
      (cond
        [(<= cp #xFFFF) (list cp)]
        [else
         (define v (- cp #x10000))
         (list (+ #xD800 (arithmetic-shift v 10))
               (+ #xDC00 (bitwise-and v #x3FF)))])))
  (define flat (apply append units))
  (define buf (make-bytes (+ (* 2 (length flat)) 2)))
  (let loop ([cs flat] [i 0])
    (cond
      [(null? cs) (void)]
      [else
       (define u (car cs))
       (bytes-set! buf i (bitwise-and u #xFF))
       (bytes-set! buf (+ i 1) (arithmetic-shift u -8))
       (loop (cdr cs) (+ i 2))]))
  buf)

(define _bomless-utf16le
  (make-ctype _bytes
              (lambda (s) (string->utf16le-bytes s))
              (lambda (b) b)))

(define CreateFileW
  (get-ffi-obj
   "CreateFileW" kernel32
<<<<<<< HEAD
   (_fun _bomless-utf16le        ; lpFileName
         _uint32                 ; dwDesiredAccess (FILE_LIST_DIRECTORY)
         _uint32                 ; dwShareMode
         _pointer                ; lpSecurityAttributes
         _uint32                 ; dwCreationDisposition
         _uint32                 ; dwFlagsAndAttributes
         _HANDLE                 ; hTemplateFile (NULL = 0)
=======
   (_fun _bomless-utf16le
         _uint32
         _uint32
         _pointer
         _uint32
         _uint32
         _HANDLE
>>>>>>> feat/fs-live-watch-20261105
         -> _HANDLE)))

(define CloseHandle
  (get-ffi-obj "CloseHandle" kernel32 (_fun _HANDLE -> _int32)))

(define ReadDirectoryChangesW
  (get-ffi-obj
   "ReadDirectoryChangesW" kernel32
<<<<<<< HEAD
   (_fun _HANDLE                 ; hDirectory
         _pointer                ; lpBuffer
         _uint32                 ; nBufferLength
         _int32                  ; bWatchSubtree
         _uint32                 ; dwNotifyFilter
         _pointer                ; lpBytesReturned
         _pointer                ; lpOverlapped
         _pointer                ; lpCompletionRoutine
         -> _int32)))

(define FILE_LIST_DIRECTORY          #x0001)
(define OPEN_EXISTING                3)
(define FILE_SHARE_READ              #x1)
(define FILE_SHARE_WRITE             #x2)
(define FILE_SHARE_DELETE            #x4)
(define FILE_FLAG_BACKUP_SEMANTICS   #x02000000)
(define FILE_NOTIFY_CHANGE_FILE_NAME  #x1)
(define FILE_NOTIFY_CHANGE_DIR_NAME   #x2)
(define FILE_NOTIFY_CHANGE_LAST_WRITE #x10)

(define (open-directory-watch! directory)
  (define handle
    (CreateFileW directory
                 FILE_LIST_DIRECTORY
                 (+ FILE_SHARE_READ FILE_SHARE_WRITE FILE_SHARE_DELETE)
                 #f
                 OPEN_EXISTING
                 FILE_FLAG_BACKUP_SEMANTICS
                 0))
  (if (= handle -1) #f handle))

;; UTF-16LE code units from a FILE_NOTIFY_INFORMATION buffer, surrogate
;; pairs combined so astral-plane filenames survive.
=======
   (_fun _HANDLE
         _pointer
         _uint32
         _int32
         _uint32
         _pointer
         _pointer
         _pointer
         -> _int32)))

(define (open-directory-watch! directory)
  (define handle
    (CreateFileW directory 1 7 #f 3 #x2000000 0))
  (if (= handle -1) #f handle))

>>>>>>> feat/fs-live-watch-20261105
(define (utf16-region->string buffer start end)
  (let loop ([i start] [chars '()])
    (cond
      [(>= i end) (list->string (reverse chars))]
      [else
<<<<<<< HEAD
       (define unit (bitwise-ior (bytes-ref buffer i)
                                 (arithmetic-shift (bytes-ref buffer (+ i 1)) 8)))
=======
       (define unit
         (bitwise-ior (bytes-ref buffer i)
                      (arithmetic-shift (bytes-ref buffer (+ i 1)) 8)))
>>>>>>> feat/fs-live-watch-20261105
       (define low
         (and (>= unit #xD800) (<= unit #xDBFF) (<= (+ i 4) end)
              (bitwise-ior (bytes-ref buffer (+ i 2))
                           (arithmetic-shift (bytes-ref buffer (+ i 3)) 8))))
       (cond
         [(and (>= low #xDC00) (<= low #xDFFF))
          (define cp (+ #x10000
                        (arithmetic-shift (- unit #xD800) 10)
                        (- low #xDC00)))
          (loop (+ i 4) (cons (integer->char cp) chars))]
         [else
          (loop (+ i 2) (cons (integer->char unit) chars))])])))

<<<<<<< HEAD
;; Little-endian uint32 from the notification buffer.
=======
>>>>>>> feat/fs-live-watch-20261105
(define (le32 buffer offset)
  (bitwise-ior (bytes-ref buffer offset)
               (arithmetic-shift (bytes-ref buffer (+ offset 1)) 8)
               (arithmetic-shift (bytes-ref buffer (+ offset 2)) 16)
               (arithmetic-shift (bytes-ref buffer (+ offset 3)) 24)))

<<<<<<< HEAD
;; Walks one notification buffer: changed native paths, possibly
;; duplicated (renames contribute both halves).
(define (parse-changes! buffer byte-count root)
  (let loop ([offset 0] [paths '()])
    (if (>= offset byte-count)
        (reverse paths)
        (let* ([next (le32 buffer offset)]
               [name-len (le32 buffer (+ offset 8))]
               [name (utf16-region->string buffer
                                           (+ offset 12)
                                           (+ offset 12 name-len))]
               [full (string-append root "\\" name)])
          (cond
            [(zero? next) (cons full paths)]
            [else (loop (+ offset next) (cons full paths))])))))

(define (read-directory-changes! handle buffer buffer-size root)
  (define bytes-returned (malloc _uint32 'raw))
  (define ok (ReadDirectoryChangesW handle
                                    buffer buffer-size
                                    1   ; bWatchSubtree: the whole root
                                    (+ FILE_NOTIFY_CHANGE_FILE_NAME
                                       FILE_NOTIFY_CHANGE_DIR_NAME
                                       FILE_NOTIFY_CHANGE_LAST_WRITE)
                                    bytes-returned
                                    #f
                                    #f))
  (cond
    [(zero? ok) #f]
    [else
     (define n (ptr-ref bytes-returned _uint32))
     (cond
       [(zero? n) 'overflow]
       [else (parse-changes! buffer n root)])]))

;; Stop markers are our own wakeup artifacts, not user activity.
(define (stop-marker? path)
  (regexp-match? #px"\\\\\\.fulcrum-stop-" path))

(module+ main
  (define ch (place-channel))
=======
(define (parse-changes! buffer byte-count root)
  (let loop ([offset 0] [paths '()])
    (cond
      [(>= offset byte-count) (reverse paths)]
      [else
       (define next (le32 buffer offset))
       (define name-len (le32 buffer (+ offset 8)))
       (define name
         (utf16-region->string buffer (+ offset 12) (+ offset 12 name-len)))
       (define full (string-append root "\\" name))
       (cond
         [(zero? next) (cons full paths)]
         [else (loop (+ offset next) (cons full paths))])])))

(define (read-directory-changes! handle buffer buffer-size root)
  (define bytes-returned (malloc _uint32 'raw))
  (define ok
    (ReadDirectoryChangesW handle buffer buffer-size 1 #x13
                           bytes-returned #f #f))
  (cond
    [(zero? ok) #f]
    [(zero? (ptr-ref bytes-returned _uint32)) 'overflow]
    [else (parse-changes! buffer (ptr-ref bytes-returned _uint32) root)]))

;; Stop markers are our own wakeup artifacts, not user activity.
(define (stop-marker? path)
  (regexp-match? #px"[\\\\/]\\.fulcrum-stop-" path))

(define (main ch)
  ;; ch carries event batches to the dispatcher. A dedicated control
  ;; channel is handed back over ch for stop orders - otherwise the
  ;; dispatcher's reader thread and this place race for messages on the
  ;; single channel (the dispatcher could swallow the stop order).
>>>>>>> feat/fs-live-watch-20261105
  (define root (place-channel-get ch))
  (define handle (open-directory-watch! root))
  (when (not handle)
    (place-channel-put ch 'open-failed)
    (exit 0))
  (place-channel-put ch 'ready)
  (let watch-loop ([buffer (make-bytes 65536)])
    (cond
<<<<<<< HEAD
      [(equal? 'stop (sync/timeout 0 ch))
       (with-handlers ([exn:fail? (lambda (_) (void))])
         (CloseHandle handle))]
=======
      ;; Non-blocking check for the stop order between reads.
      [(equal? 'stop (sync/timeout 0 ch))
       (CloseHandle handle)]
>>>>>>> feat/fs-live-watch-20261105
      [else
       (define result
         (read-directory-changes! handle buffer
                                  (bytes-length buffer) root))
       (cond
<<<<<<< HEAD
         [(not result) (void)]
         [(eq? result 'overflow)
          (place-channel-put ch 'overflow)
           (watch-loop buffer)])]))
  (exit 0))
=======
         [(eq? result 'overflow)
          (place-channel-put ch 'overflow)
          (watch-loop buffer)]
         [else
          (define reportable
            (for/list ([p (in-list (or result '()))]
                       #:unless (stop-marker? p))
              p))
          (unless (null? reportable)
            (place-channel-put ch reportable))
          (watch-loop buffer)])])))

(provide main)
>>>>>>> feat/fs-live-watch-20261105
