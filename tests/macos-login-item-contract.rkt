#lang racket/base

;; The deprecated v0.14.1 release PR #104 contained an unsynced macOS
;; Login Items control. Keep its actual native integration when rescuing
;; features onto current main without importing the obsolete release tag.
(require rackunit racket/file racket/runtime-path racket/string)

(define-runtime-path settings "../macos-host/Sources/RivetHost/SettingsView.swift")
(define-runtime-path acceptance "../scripts/e2e/macos-ax.swift")
(define source (file->string settings))
(define e2e (file->string acceptance))

(test-case "login item is native per-device state, not a backend setting"
  (check-true (string-contains? source "import ServiceManagement"))
  (check-true (string-contains? source "SMAppService.mainApp.status"))
  (check-true (string-contains? source "SMAppService.mainApp.register()"))
  (check-true (string-contains? source "SMAppService.mainApp.unregister()"))
  (check-true (string-contains? source "refreshLoginItem()"))
  (check-true (string-contains? source "SMAppService.openSystemSettingsLoginItems()"))
  (check-false (string-contains? source "settingsSet(key: \"start-at-login\""))
  (check-true (string-contains? source "settings-start-at-login"))
  (check-true (string-contains? e2e "findByIdentifier(settingsWindow, \"settings-start-at-login\")")))
