#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; Static gate for launcher geometry on mixed-DPI multi-monitor desks
;; (design/DESIGN.md "启动窗几何与多显示器"). Every summon follows the
;; pointer's monitor and clamps the 680x640 design size to that monitor's
;; work area. Windows must re-assert its monitor-aware geometry after
;; WM_DPICHANGED: WinUI applies the system suggested rect (the fresh
;; physical size scaled by the DPI ratio a second time), which made the
;; first cross-monitor summon tiny and only the second one correct.

(define-runtime-path windows-path "../windows/MainWindow.xaml.cpp")
(define-runtime-path linux-path "../linux/src/main.cpp")
(define-runtime-path macos-path
  "../macos-host/Sources/RivetHost/RivetHostApp.swift")
(define-runtime-path design-path "../design/DESIGN.md")

(define (read-source path)
  (string-replace (file->string path) "\r\n" "\n"))

(define windows-source (read-source windows-path))
(define linux-source (read-source linux-path))
(define macos-source (read-source macos-path))
(define design-source (read-source design-path))

;; ---- Windows: PerMonitorV2 math + the DPI-change re-assert ----------------

(check-true (string-contains? windows-source "MDT_EFFECTIVE_DPI")
            "position_launcher must size from the target monitor's effective DPI")
(check-true (string-contains? windows-source "MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)")
            "every summon must follow the pointer's monitor, not the primary work area")
(check-true (string-contains? windows-source "if (msg == WM_DPICHANGED && self != nullptr)")
            "the window subclass must handle WM_DPICHANGED")
;; The handler lets WinUI update its rasterization scale first, then
;; re-asserts the monitor-aware geometry — swallowing the message outright
;; would strand the XAML island at the old scale.
(define dpi-handler
  (let ([start (string-find windows-source "if (msg == WM_DPICHANGED && self != nullptr)")])
    (substring windows-source start (+ start 1400))))
(check-true (string-contains? dpi-handler "DefSubclassProc(hWnd, msg, wParam, lParam)")
            "WM_DPICHANGED must chain to WinUI before repositioning")
(check-true (string-contains? dpi-handler "position_launcher(hWnd)")
            "WM_DPICHANGED must re-assert the monitor-aware geometry")

;; ---- Linux: pointer monitor + design-v2 clamp ------------------------------

(check-true (string-contains? linux-source "void place_panel_on_pointer_monitor(GtkWindow* panel)")
            "Linux must place the panel on the pointer's monitor")
(check-true (string-contains? linux-source "std::min(680, usable.width)")
            "Linux placement must clamp the design width to the work area")
(check-true (string-contains? linux-source "std::min(640, usable.height)")
            "Linux placement must clamp the design v2 height (640), not the pre-v2 460")

;; ---- macOS: pointer screen + work-area clamp in points ---------------------

(check-true (string-contains? macos-source "let pointer = NSEvent.mouseLocation")
            "macOS must follow the pointer's screen, not NSScreen.main alone")
(check-true (string-contains? macos-source "min(680, visible.width)")
            "macOS must clamp the panel width to the target screen's visibleFrame")
(check-true (string-contains? macos-source "min(640, visible.height)")
            "macOS must clamp the panel height to the target screen's visibleFrame")

;; ---- design truth source ----------------------------------------------------

(check-true (string-contains? design-source "启动窗几何与多显示器")
            "DESIGN.md must declare the launcher geometry / multi-display contract")
(check-true (string-contains? design-source "WM_DPICHANGED")
            "DESIGN.md must declare the Windows DPI-change re-assert rule")
(check-true (string-contains? design-source "X11 无逐显示器缩放")
            "DESIGN.md must declare the X11 per-monitor scaling limitation")