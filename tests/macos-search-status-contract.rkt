#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; Regression gate for GitHub #199: a newer query cancels an obsolete one,
;; which is normal rather than a user-visible search failure. The macOS
;; launcher also must not pin transient errors/notices indefinitely.
(define-runtime-path macos-path
  "../macos-host/Sources/RivetHost/ContentView.swift")
(define source (file->string macos-path))

(check-true
 (string-contains? source "if error is CancellationError")
 "Swift task cancellation is an expected no-op")

(check-true
 (string-contains? source "message.contains(\"request cancelled\")")
 "RVT1 request-cancelled errors must remain silent")

(check-true
 (string-contains? source "!Self.isExpectedSearchCancellation(error)")
 "the search catch handler must suppress the engine's normal cancellation")

(check-true
 (string-contains? source "guard generation == self?.searchGeneration")
 "stale searches must not overwrite newer search state")

(check-true
 (string-contains? source "statusExpiryTask?.cancel()")
 "each newer status must cancel expiry of the previous message")

(check-true
 (string-contains? source "Task.sleep(nanoseconds: 10_000_000_000)")
 "transient statuses need a bounded display lifetime")

(check-true
 (string-contains? source "self.status == message")
 "an expired status must not overwrite a newer notification")

(check-true
 (string-contains? source "self.status = self.t(\"readyStatusMac\")")
 "expired messages return to the localized ready state")

(check-true
 (string-contains? source "case \"notify\":")
 "notification delivery must continue updating the status line")
