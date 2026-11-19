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
  ;; Holding pending_dirs entries forever reconciled each directory at
  ;; most once per session: every later change in it was silently dropped.
  (check-true (string-contains? cpp "state->pending_dirs.erase(directory);")))

(test-case "delete-pending re-checks span the handle-release window"
  ;; A delete notification can fire while another process still holds the
  ;; file (delete-pending); the vanish after the handle releases carries
  ;; no second notification. The 2/4/6/8 s ladder mirrors the backend's
  ;; proven Racket flusher model.
  (check-true (string-contains? h "recheck;"))
  (check-true (string-contains? cpp "scheduled + 2000"))
  (check-true (string-contains? cpp "scheduled + 4000"))
  (check-true (string-contains? cpp "scheduled + 6000"))
  (check-true (string-contains? cpp "scheduled + 8000")))

(test-case "Stop never joins worker threads on the caller thread"
  ;; The reconcile worker can sit inside a synchronous fs-reconcile RPC
  ;; while the backend is busy scanning (SQLite write locks). Joining it
  ;; from QuitApplication froze the UI thread and wedged the process
  ;; (packaged acceptance: main HWND stopped pumping). Stop must signal,
  ;; then hand the joins to a detached cleanup thread that owns the shared
  ;; state, so the owning window can die while an RPC is still in flight.
  (define stop-body
    (let ([start (string-find cpp "void FsWatchHost::Stop()")])
      (substring cpp start
                 (string-find cpp "void FsWatchHost::WatchRootLoop"))))
  (check-true (string-contains? stop-body "NEVER join on the caller thread"))
  (check-true (string-contains? stop-body "std::thread([state]() mutable {"))
  (check-true (string-contains? stop-body ".detach();"))
  (check-true (string-contains? stop-body "state->reconcile_thread.join();"))
  ;; Workers capture the shared state, never `this`: they must outlive the
  ;; owning FsWatchHost (a MainWindow member) safely.
  (check-true (string-contains? h "std::shared_ptr<State> state_;"))
  (check-true (string-contains? h "static void ReconcileLoop(std::shared_ptr<State> state);"))
  (check-true (string-contains? cpp "std::thread([state]() { ReconcileLoop(state); });")))