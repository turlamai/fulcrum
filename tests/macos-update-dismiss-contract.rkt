#lang racket/base

;; Product-level guard against the macOS update panel rendering a permanent
;; spinner after Close. CI also compiles the real SwiftUI/AppKit host and
;; packaged macOS acceptance exercises the native host build.
(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

(define-runtime-path updater "../macos-host/Sources/RivetHost/UpdateController.swift")
(define-runtime-path view "../macos-host/Sources/RivetHost/UpdatePanelView.swift")
(define-runtime-path host "../macos-host/Sources/RivetHost/RivetHostApp.swift")

(define controller (file->string updater))
(define panel (file->string view))
(define app (file->string host))

(test-case "all update Close controls dismiss the actual native panel"
  (check-false (string-contains? panel "controller.panelVisible = false"))
  (check-true (>= (length (regexp-match* #rx"controller\\.dismissPanel\\(\\)" panel)) 5)
              "checking, up-to-date, available, failed, and downloading Close paths")
  (check-true (string-contains? controller "AppDelegate.shared?.hideUpdatePanel()"))
  (check-true (string-contains? app "updateWindow?.orderOut(nil)")))

(test-case "AppKit titlebar close follows the same dismissal path"
  (check-true (string-contains? app "NSWindowDelegate"))
  (check-true (string-contains? app "window.delegate = self"))
  (check-true (string-contains? app "closing === updateWindow"))
  (check-true (string-contains? app "updateController?.dismissPanel()")))

(test-case "idle is never an active Checking spinner"
  (check-true (string-contains? panel "case .idle:\n                idle"))
  (check-true (string-contains? panel "case .checking:\n                checking"))
  (check-true (string-contains? panel "controller.t(\"updateCheckNow\")"))
  (check-true (string-contains? panel "controller.checkForUpdates()"))
  (check-true (string-contains? panel "guard panelVisible else { return }")))

(test-case "a dismissed asynchronous check cannot repaint or re-open the panel"
  (check-true (string-contains? controller "private var checkGeneration = 0"))
  (check-true (string-contains? controller "checkGeneration &+= 1"))
  (check-true (string-contains? controller "manualCheckTask?.cancel()"))
  (check-true (string-contains? controller "manualCheckTask = nil"))
  (check-true (>= (length (regexp-match* #rx"guard generation == checkGeneration else" controller)) 2)
              "both a successful feed response and an error must honor Close")
  (check-true (string-contains? app "guard controller.panelVisible else { return }"))
  (check-true (string-contains? controller
                                "guard phase != .downloading && phase != .checking else { return }")))
