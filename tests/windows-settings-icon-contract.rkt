#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; Static gate for the Windows Settings title-bar icon (issue #140). The
;; String overload of AppWindow.SetIcon expects an .ico file path; passing
;; an exe path fails silently inside the constructor's try/catch and the
;; slot stays blank. The pinned rivet ships ApplyAppIcon (winui_window.hpp):
;; LoadIconW on the module's RT_GROUP_ICON resource ID 1, then
;; SetIcon(IconId). The Windows packaged acceptance exercises the pixels;
;; this file locks the source contract on every CI platform.
(define-runtime-path source-path "../windows/SettingsWindow.xaml.cpp")
(define-runtime-path header-path "../windows/SettingsWindow.xaml.h")
(define source (file->string source-path))
(define header (file->string header-path))

(check-true
 (string-contains? source "(void)rivet::system::ApplyAppIcon(*this);")
 "the Settings window must apply its icon through the pinned rivet helper")

(check-false
 (string-contains? source "SetIcon(exe_path)")
 "SetIcon(String) wants an .ico path — an exe path silently failed (#140)")

(check-false
 (string-contains? source "GetModuleFileNameW")
 "the icon comes from the module resource table, not the exe file path")

;; LoadIconW returns a shared, module-owned handle; destroying it would
;; break the icon for every other consumer of the resource.
(check-false
 (string-contains? source "DestroyIcon")
 "module-owned shared icons must not be destroyed")

(check-true
 (string-contains? header "#include \"winui_window.hpp\"")
 "ApplyAppIcon lives in rivet's winui_window.hpp — include it explicitly")
