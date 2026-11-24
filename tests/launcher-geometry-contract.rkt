#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; Static gate for launcher geometry on mixed-DPI multi-monitor desks
;; (design/DESIGN.md "启动窗几何与多显示器"). Every summon follows the
;; pointer's monitor and fits the 680x640 design ceiling to that monitor's
;; work area with margins: at most 55% of its width and 65% of its height
;; (clamping alone left the panel covering 85% of a 1280x800 screen). All
;; three hosts apply the same shares. Windows must re-assert its
;; monitor-aware geometry after WM_DPICHANGED: WinUI applies the system
;; suggested rect (the fresh physical size scaled by the DPI ratio a second
;; time), which made the first cross-monitor summon tiny and only the second
;; one correct.

(define-runtime-path windows-path "../windows/MainWindow.xaml.cpp")
(define-runtime-path linux-path "../linux/src/main.cpp")
(define-runtime-path macos-path
  "../macos-host/Sources/RivetHost/RivetHostApp.swift")
(define-runtime-path design-path "../design/DESIGN.md")
(define-runtime-path prototype-css-path "../design/prototype/shared.css")

(define (read-source path)
  (string-replace (file->string path) "\r\n" "\n"))

(define windows-source (read-source windows-path))
(define linux-source (read-source linux-path))
(define macos-source (read-source macos-path))
(define design-source (read-source design-path))
(define prototype-css (read-source prototype-css-path))

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
;; The fit compares physical pixels with physical pixels: the design size is
;; scaled by the target monitor's DPI before the percentage cap.
(check-true (string-contains? windows-source "workWidth * kWorkWidthPercent / 100")
            "Windows must cap the panel at kWorkWidthPercent of the work-area width")
(check-true (string-contains? windows-source "workHeight * kWorkHeightPercent / 100")
            "Windows must cap the panel at kWorkHeightPercent of the work-area height")
(check-true (string-contains? windows-source "constexpr int kWorkWidthPercent = 55;")
            "the Windows width share must match the design contract (55%)")
(check-true (string-contains? windows-source "constexpr int kWorkHeightPercent = 65;")
            "the Windows height share must match the design contract (65%)")
(check-true (string-contains? windows-source "LauncherSize const size = fit_launcher_size(workWidth, workHeight, dpiX, dpiY)")
            "position_launcher must summon with the fitted size, not the raw design size")
(check-false (string-contains? windows-source "std::min(workWidth, ::MulDiv(kWindowWidth")
             "the bare work-area clamp is not the sizing rule any more")

;; ---- Linux: pointer monitor + the shared fit -------------------------------

(check-true (string-contains? linux-source "void place_panel_on_pointer_monitor(GtkWindow* panel)")
            "Linux must place the panel on the pointer's monitor")
(check-true (string-contains? linux-source "std::min(680, work.width * 55 / 100)")
            "Linux placement must fit the design width to 55% of the work area")
(check-true (string-contains? linux-source "std::min(640, work.height * 65 / 100)")
            "Linux placement must fit the design v2 height (640) to 65%, not the pre-v2 460")
(check-true (string-contains? linux-source "PanelSize const size = fit_panel_size(usable);")
            "the X11 summon must pass the fitted size to GTK and the placement math")

;; ---- macOS: pointer screen + the shared fit in points ---------------------

(check-true (string-contains? macos-source "let pointer = NSEvent.mouseLocation")
            "macOS must follow the pointer's screen, not NSScreen.main alone")
(check-true (string-contains? macos-source "min(680, visible.width * 0.55)")
            "macOS must fit the panel width to 55% of the target screen's visibleFrame")
(check-true (string-contains? macos-source "min(640, visible.height * 0.65)")
            "macOS must fit the panel height to 65% of the target screen's visibleFrame")

;; ---- design truth source ----------------------------------------------------

(check-true (string-contains? design-source "启动窗几何与多显示器")
            "DESIGN.md must declare the launcher geometry / multi-display contract")
(check-true (string-contains? design-source "宽度 ≤ 工作区 55%、高度 ≤ 工作区 65%")
            "DESIGN.md must declare the 55% / 65% work-area fit")
(check-true (string-contains? design-source "WM_DPICHANGED")
            "DESIGN.md must declare the Windows DPI-change re-assert rule")
(check-true (string-contains? design-source "X11 无逐显示器缩放")
            "DESIGN.md must declare the X11 per-monitor scaling limitation")
(check-true (string-contains? prototype-css "width: min(680px, 55vw)")
            "the prototype panel must mirror the 55% width fit")
(check-true (string-contains? prototype-css "max-height: min(640px, 65dvh)")
            "the prototype panel must mirror the 65% height fit")