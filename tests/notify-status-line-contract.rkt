#lang racket/base

(require rackunit
         racket/contract
         racket/file
         racket/runtime-path
         racket/string)

;; Static gate for the notify status line on all three hosts. The backend
;; delivers run-action feedback (update check results, settings toggles,
;; plugin installs) as a "notify" event via emit-events!; every host must
;; write that payload into its status line. All three hosts originally
;; handled copy/paste/ai-finished/open-url/update-available but dropped
;; "notify", so the update check ran while the status line kept its
;; "Running…" placeholder forever (found during the v0.15.1 VM pass:
;; the deb's update-last-check advanced while the row never refreshed).
(define-runtime-path linux-path "../linux/src/main.cpp")
(define-runtime-path windows-path "../windows/MainWindow.xaml.cpp")
(define-runtime-path macos-path
  "../macos-host/Sources/RivetHost/ContentView.swift")
(define linux-source (file->string linux-path))
(define windows-source (file->string windows-path))
(define macos-source (file->string macos-path))

(define/contract (notify-branch? source)
  (-> string? boolean?)
  (and (string-contains? source "\"notify\"")
       (or (string-contains? source "set_status(payload)")
           (string-contains? source "SetStatusOk(to_wide(payload))")
           (string-contains? source "status = payload"))))

(check-true (notify-branch? linux-source)
            "the Linux host must write notify events into the status line")
(check-true (notify-branch? windows-source)
            "the Windows host must write notify events into the status line")
(check-true (notify-branch? macos-source)
            "the macOS host must write notify events into the status line")

;; The backend keeps emitting action feedback as notify events; the wire
;; contract test (backend-wire-test.rkt) covers the runtime side. This
;; file only locks the host-side branches.
(check-true
 (string-contains? linux-source "handle_event")
 "the Linux dispatch point moved; repin this contract to the new handler")
