#lang racket/base

(require rackunit racket/file racket/runtime-path racket/string)

(define-runtime-path smoke "../scripts/acceptance/windows-msi-upgrade-smoke.ps1")
(define source (file->string smoke))

(test-case "real MSI upgrade must verify installed executable and native backend"
  (check-true (string-contains? source "Assert-InstalledColdLaunch \"old-install\""))
  (check-true (string-contains? source "Assert-InstalledColdLaunch \"upgraded-install\""))
  (check-true (string-contains? source "Start-Process -FilePath $exe -PassThru"))
  (check-true (string-contains? source "ProgramFiles"))
  (check-true (string-contains? source "UIAutomation"))
  (check-true (string-contains? source "QueryBox"))
  (check-true (string-contains? source "StatusText"))
  (check-true (string-contains? source "45s"))
  (check-true (string-contains? source "Get-FulcrumInstallation"))
  (check-true (string-contains? source "$desktop.FindAll("))
  (check-true (string-contains? source "LastStatus"))
  (check-true (string-contains? source "WindowCount"))
  (check-true (string-contains? source "startup-"))
  (check-true (string-contains? source "Installed launch diagnostic"))
  ;; A real backend can replace its initial Ready footer with an
  ;; authenticated update-available notice before UI Automation attaches.
  (check-true (string-contains? source "Update available:"))
  (check-true (string-contains? source "发现新版本："))
  ;; Reject arbitrary non-starting errors; only exact success state
  ;; prefixes satisfy installed launcher readiness.
  (check-true (string-contains? source "Ready|就绪|")))
