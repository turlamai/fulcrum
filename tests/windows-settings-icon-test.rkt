#lang racket/base

;; This static contract catches wrong WinUI title-bar icon overloads on
;; Linux/macOS CI before the packaged Windows WM_GETICON E2E executes.
(require racket/file
         racket/runtime-path
         racket/string
         rackunit)

(define-runtime-path settings-window "../windows/SettingsWindow.xaml.cpp")
(define source (file->string settings-window))

(test-case "Windows Settings installs its packaged branded icon through Rivet"
  (check-true (string-contains? source "#include \"../system/winui_window.hpp\""))
  (check-true (string-contains? source "rivet::system::ApplyAppIcon(*this)"))
  (check-false (regexp-match? #px"SetIcon\\s*\\(\\s*exe_path" source))
  (check-false (string-contains? source "appWindow.SetIcon(exe_path)")))
