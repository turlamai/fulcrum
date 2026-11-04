#lang racket/base

;; Static guard for the Windows native RPC ownership pattern. Runtime
;; behavior is covered by the packaged WinUI acceptance, but the bug here
;; is a worker thread calling an API reference after its MainWindow owner
;; has already been destroyed during MSI repair/shutdown.
(require racket/file
         racket/runtime-path
         racket/string
         rackunit)

(define-runtime-path main-cpp "../windows/MainWindow.xaml.cpp")
(define-runtime-path main-header "../windows/MainWindow.xaml.h")
(define-runtime-path settings-cpp "../windows/SettingsWindow.xaml.cpp")
(define-runtime-path settings-header "../windows/SettingsWindow.xaml.h")

(define main (file->string main-cpp))
(define main-h (file->string main-header))
(define settings (file->string settings-cpp))
(define settings-h (file->string settings-header))

(test-case "RPC workers retain an owning API handle, never a window-owned raw pointer"
  (check-true (string-contains? main-h "std::shared_ptr<rivet_app::API> api_;"))
  (check-true (string-contains? settings-h "std::shared_ptr<rivet_app::API> api_;"))
  (check-true (string-contains? main "auto api = api_;"))
  (check-true (string-contains? settings "auto api = api_;"))
  (check-false (string-contains? main "auto* api = api_.get();"))
  (check-false (string-contains? main "auto* api2 = window->api_.get();"))
  (check-false (string-contains? settings "auto* api = api_;"))
  (check-false (string-contains? main "Attach(api_.get())")))

(test-case "API lease keeps backend instance alive through last RPC"
  (check-true (string-contains? main "[backend = window->backend_]"))
  (check-true (string-contains? main "delete api;"))
  (check-true (string-contains? main "(void)backend;")))
