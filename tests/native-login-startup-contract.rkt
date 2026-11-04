#lang racket/base

;; Rescue the native per-device Start at login behavior from the obsolete
;; v0.14.1 PR #104, without importing its stale release/workflow changes.
(require rackunit racket/file racket/runtime-path racket/string)

(define-runtime-path win-app "../windows/App.xaml.cpp")
(define-runtime-path win-main "../windows/MainWindow.xaml.cpp")
(define-runtime-path win-header "../windows/Autostart.h")
(define-runtime-path win-settings "../windows/SettingsWindow.xaml.cpp")
(define-runtime-path win-xaml "../windows/SettingsWindow.xaml")
(define-runtime-path linux-main "../linux/src/main.cpp")
(define-runtime-path linux-header "../linux/src/autostart.hpp")
(define-runtime-path linux-test "../tests/linux-ui-test.cpp")
(define (read path) (file->string path))

(test-case "Windows login startup stays silent and per-user"
  (check-true (string-contains? (read win-header) "HKEY_CURRENT_USER"))
  (check-true (string-contains? (read win-header) "RegSetValueExW"))
  (check-true (string-contains? (read win-header) "RegDeleteValueW"))
  (check-true (string-contains? (read win-header) " --background"))
  (check-true (string-contains? (read win-app) "BackgroundLaunch()"))
  (check-true (string-contains? (read win-main) "BackgroundLaunch()"))
  (check-true (string-contains? (read win-settings) "SetAutostart(toggle.IsOn())"))
  (check-true (string-contains? (read win-settings) "return;  // never send machine-local startup state over settings-set"))
  (check-true (string-contains? (read win-xaml) "FulcrumStartAtLogin")))

(test-case "Linux XDG login startup is local and opt-in"
  (check-true (string-contains? (read linux-header) "XDG_CONFIG_HOME"))
  (check-true (string-contains? (read linux-header) "site.jrtx.fulcrum.desktop"))
  (check-true (string-contains? (read linux-header) "fulcrum_desktop_exec_quote"))
  (check-true (string-contains? (read linux-header) " --background"))
  (check-true (string-contains? (read linux-main) "fulcrum_set_autostart(enabled, executable, error)"))
  (check-true (string-contains? (read linux-main) "if (login_background) return 0"))
  (check-true (string-contains? (read linux-main) "g_login_background_start"))
  (check-true (string-contains? (read linux-test) "g_dir_make_tmp"))
  (check-true (string-contains? (read linux-test) "fulcrum_set_autostart(false")))
