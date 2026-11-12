#lang racket/base

(require rackunit
         racket/contract
         racket/file
         racket/runtime-path
         racket/string)

;; Static gate for the design v2 baseline (commit 0ca2178, issue #220):
;; IME composition guard, focus contract, hit highlighting, masked
;; sensitive clipboard rows and the dynamic panel height must exist on all
;; three hosts. The packaged acceptances exercise the pixels; this file
;; locks the source contract on every CI platform, like the other
;; *-contract tests.

(define-runtime-path linux-path "../linux/src/main.cpp")
(define-runtime-path linux-header "../linux/GeneratedBackend.hpp")
(define-runtime-path windows-path "../windows/MainWindow.xaml.cpp")
(define-runtime-path windows-header "../windows/MainWindow.xaml.h")
(define-runtime-path macos-path
  "../macos-host/Sources/RivetHost/ContentView.swift")
(define-runtime-path macos-app "../macos-host/Sources/RivetHost/RivetHostApp.swift")
(define-runtime-path engine-path "../app/core/engine.rkt")
(define-runtime-path clipboard-path "../app/core/clipboard.rkt")
(define-runtime-path settings-path "../app/core/settings.rkt")
(define-runtime-path schema-path "../rivet-schema.json")
(define-runtime-path i18n-en "../shared/i18n/en.json")
(define-runtime-path i18n-zh "../shared/i18n/zh.json")

(define linux (file->string linux-path))
(define linux-backend (file->string linux-header))
(define windows (file->string windows-path))
(define windows-backend (file->string windows-header))
(define macos (string-append (file->string macos-path)
                             (file->string macos-app)))
(define engine (file->string engine-path))
(define clipboard (file->string clipboard-path))
(define settings (file->string settings-path))
(define schema (file->string schema-path))
(define en (file->string i18n-en))
(define zh (file->string i18n-zh))

;; --- IME composition guard (invariant 7) ---
(check-true (string-contains? macos "hasMarkedText()")
            "macOS guard reads the field editor's marked range")
(check-true (string-contains? windows "ImmGetContext")
            "Windows guard reads the thread IMM32 context")
(check-true (string-contains? linux "preedit-changed")
            "GTK guard tracks the IM context preedit")

;; --- focus contract (invariant 8) ---
(check-true (string-contains? macos "listFocusRequest")
            "macOS moves focus to the results list as one tab stop")
(check-true (string-contains? windows "ResultsList().Focus(")
            "Windows moves focus to the results list")
(check-true (string-contains? linux "grab_focus(GTK_WIDGET(launcher.results))")
            "GTK moves focus to the results list")
(check-true (string-contains? macos "actionParent")
            "macOS remembers the originating actions row")
(check-true (string-contains? windows "pending_restore_id_")
            "Windows restores the originating row by item id")
(check-true (string-contains? linux "actions_parent_id")
            "GTK restores the originating row by item id")

;; --- hit highlighting (invariant 6) ---
(check-true (string-contains? engine "x:sensitive")
            "engine meta encoding carries the sensitive flag")
(check-true (string-contains? engine "fuzzy-match-offsets")
            "engine computes hit offsets with the scoring alignment")
(check-true (string-contains? macos "hitHighlighted")
            "macOS renders matched runs in accent-text")
(check-true (string-contains? windows "fill_highlighted")
            "Windows renders matched runs in accent-text")
(check-true (string-contains? linux "apply_hit_attributes")
            "GTK renders matched runs in accent-text")

;; --- sensitive clipboard rows (invariant 9) ---
(check-true (string-contains? clipboard "sensitive-text?")
            "clipboard detection masks credential-like content")
(check-true (string-contains? schema "clipboard-reveal")
            "the local-only reveal RPC is part of the schema")
(check-true (string-contains? macos "clipboardReveal")
            "macOS wires the reveal RPC")
(check-true (string-contains? windows "clipboard_reveal")
            "Windows wires the reveal RPC")
(check-true (string-contains? linux "clipboard_reveal")
            "GTK wires the reveal RPC")
(check-true (string-contains? en "\"sensitiveBadge\"")
            "en carries the Sensitive badge string")
(check-true (string-contains? zh "\"sensitiveBadge\"")
            "zh carries the Sensitive badge string")

;; --- dynamic panel height (12-row cap) ---
(check-true (string-contains? macos "updatePanelHeight")
            "macOS resizes the panel with results")
(check-true (string-contains? windows "UpdatePanelHeight")
            "Windows resizes the panel with results")
(check-true (string-contains? linux "update_panel_height")
            "GTK resizes the panel with results")
(check-true (string-contains? settings "(<= v 12)")
            "max-results tops out at the 12-row visual cap")
