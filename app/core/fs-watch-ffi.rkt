#lang racket/base

;; Windows ReadDirectoryChangesW bindings for the live file watcher.
;; Loaded via dynamic-require only on Windows — kernel32.dll does not
;; exist elsewhere. The blocking-synchronous pattern (no overlapped I/O)
;; keeps the FFI surface minimal: ReadDirectoryChangesW parks the OS
;; thread until a change lands or the handle closes, so stopping is a
;; CloseHandle from another thread.

(require ffi/unsafe)

(provide open-directory-watch!
         read-directory-changes!
         close-directory-watch!
         watch-handle-open?)

(define kernel32 (ffi-lib "kernel32.dll"))

(define _HANDLE* (make-ctype _int64
                             (lambda (v) v)
                             (lambda (v) v)))

(define CreateFileW
  (get-ffi-obj
   "CreateFileW" kernel32
   (_fun _string/utf-16le        ; lpFileName (BOM-less: _string/utf-16
                                 ; prepends a BOM that Windows would read
                                 ; as part of the path)

         _uint32                 ; dwDesiredAccess (FILE_LIST_DIRECTORY)
         _uint32                 ; dwShareMode
         _pointer                ; lpSecurityAttributes
         _uint32                 ; dwCreationDisposition
         _uint32                 ; dwFlagsAndAttributes
         _HANDLE*                ; hTemplateFile
         -> _HANDLE*)))

(define CloseHandle
  (get-ffi-obj "CloseHandle" kernel32 (_fun _HANDLE* -> _int32)))

(define ReadDirectoryChangesW
  (get-ffi-obj
   "ReadDirectoryChangesW" kernel32
   (_fun _HANDLE*                ; hDirectory
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

(define INVALID_HANDLE_VALUE-LOW -1)

(define (open-directory-watch! directory)
  (define handle
    (CreateFileW directory
                 FILE_LIST_DIRECTORY
                 (+ FILE_SHARE_READ FILE_SHARE_WRITE FILE_SHARE_DELETE)
                 #f
                 OPEN_EXISTING
                 FILE_FLAG_BACKUP_SEMANTICS
                 0))
  (if (= handle INVALID_HANDLE_VALUE-LOW) #f handle))

(define (watch-handle-open? handle)
  (> handle 0))

(define (close-directory-watch! handle)
  (not (zero? (CloseHandle handle))))

;; UTF-16LE code units from a FILE_NOTIFY_INFORMATION buffer, surrogate
;; pairs combined so astral-plane filenames survive.
(define (utf16le-region->string buffer start end)
  (let loop ([i start] [chars '()])
    (cond
      [(>= i end) (list->string (reverse chars))]
      [else
       (define unit (bitwise-ior (bytes-ref buffer i)
                                 (arithmetic-shift (bytes-ref buffer (+ i 1)) 8)))
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

;; Little-endian uint32 from buffer at byte offset (FILE_NOTIFY_INFORMATION
;; fields are LE uint32s; reading them bytewise avoids any import question).
(define (le32 buffer offset)
  (bitwise-ior (bytes-ref buffer offset)
               (arithmetic-shift (bytes-ref buffer (+ offset 1)) 8)
               (arithmetic-shift (bytes-ref buffer (+ offset 2)) 16)
               (arithmetic-shift (bytes-ref buffer (+ offset 3)) 24)))

;; Walks one notification buffer. Returns (values changed-paths overflow?)
;; where changed-paths are native full paths under root, in unspecified
;; order, possibly duplicated (renames contribute both halves).
(define (parse-changes! buffer byte-count root)
  (define sep-string "\\")
  (let loop ([offset 0] [paths '()])
    (if (>= offset byte-count)
        (values (reverse paths) #f)
        (let* ([next (le32 buffer offset)]
               [name-len (le32 buffer (+ offset 8))]
               [name (utf16le-region->string buffer
                                             (+ offset 12)
                                             (+ offset 12 name-len))]
               [full (string-append root sep-string name)])
          (cond
            [(zero? next) (values (cons full (reverse paths)) #f)]
            [else (loop (+ offset next) (cons full paths))])))))

;; Blocks until at least one change lands, the handle closes (returns
;; #f), or the kernel had to drop events because the buffer filled
;; (returns 'overflow — the caller must resynchronize the root).
;; Returns a list of native full paths that changed — including both
;; halves of renames — in unspecified order, possibly duplicated.
(define (read-directory-changes! handle buffer buffer-size root)
  (define bytes-returned (malloc _uint32 'raw))
  (define ok (ReadDirectoryChangesW handle
                                    buffer buffer-size
                                    1   ; bWatchSubtree: the whole root
                                    (+ FILE_NOTIFY_CHANGE_FILE_NAME
                                       FILE_NOTIFY_CHANGE_DIR_NAME
                                       FILE_NOTIFY_CHANGE_LAST_WRITE)
                                    bytes-returned
                                    #f  ; no overlapped I/O
                                    #f)) ; no completion routine
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
