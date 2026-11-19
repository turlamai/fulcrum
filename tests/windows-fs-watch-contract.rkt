#lang racket/base

;; Static guard for the Windows native filesystem watcher (FsWatchHost).
;; Runtime behavior is covered by the packaged WinUI acceptance; the bugs
;; pinned here are invisible to Racket unit tests because the watcher
;; lives in the host process, outside the embedded runtime (issue #211).
(require racket/file
         racket/runtime-path
         racket/string
         rackunit)

(define-runtime-path watch-cpp "../windows/FsWatchHost.cpp")
(define-runtime-path watch-h "../windows/FsWatchHost.h")
(define cpp (file->string watch-cpp))
(define h (file->string watch-h))

(test-case "every notification action reconciles its directory"
  ;; REMOVED is the only event a pure deletion produces; a cross-directory
  ;; move delivers RENAMED_NEW_NAME to the destination while the old-name
  ;; half only names the source. Dropping either left deleted files
  ;; searchable and moved files missing at the target.
  (for ([action (in-list '("FILE_ACTION_ADDED"
                           "FILE_ACTION_REMOVED"
                           "FILE_ACTION_MODIFIED"
                           "FILE_ACTION_RENAMED_OLD_NAME"
                           "FILE_ACTION_RENAMED_NEW_NAME"))])
    (check-true (string-contains? cpp action) action)))

(test-case "the dedupe slot is released when a directory leaves the batch"
  ;; Holding pending_dirs_ entries forever reconciled each directory at
  ;; most once per session: every later change in it was silently dropped.
  (check-true (string-contains? cpp "pending_dirs_.erase(directory);")))

(test-case "delete-pending re-checks span the handle-release window"
  ;; A delete notification can fire while another process still holds the
  ;; file (delete-pending); the vanish after the handle releases carries
  ;; no second notification. The 2/4/6/8 s ladder mirrors the backend's
  ;; proven Racket flusher model.
  (check-true (string-contains? h "recheck_;"))
  (check-true (string-contains? cpp "scheduled + 2000"))
  (check-true (string-contains? cpp "scheduled + 4000"))
  (check-true (string-contains? cpp "scheduled + 6000"))
  (check-true (string-contains? cpp "scheduled + 8000")))