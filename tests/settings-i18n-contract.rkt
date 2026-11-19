#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string
         json)

;; Static gate for the settings-page parity work (design/DESIGN.md "设置 IA"
;; and "更新交互"): one shared/i18n source, six sidebar destinations on every
;; host, the declared General row order, and one dedicated update window
;; reached from every entry point. The hosts drifted apart (Linux hardcoded
;; English chrome, Windows carried an inline update card, macOS hardcoded
;; workflow notices); this contract pins the repaired invariants so a future
;; edit cannot silently reintroduce the drift.

(define-runtime-path en-path "../shared/i18n/en.json")
(define-runtime-path zh-path "../shared/i18n/zh.json")
(define-runtime-path linux-path "../linux/src/main.cpp")
(define-runtime-path windows-settings-path "../windows/SettingsWindow.xaml.cpp")
(define-runtime-path windows-xaml-path "../windows/SettingsWindow.xaml")
(define-runtime-path windows-update-path "../windows/UpdateWindow.cpp")
(define-runtime-path windows-main-path "../windows/MainWindow.xaml.cpp")
(define-runtime-path macos-settings-path
  "../macos-host/Sources/RivetHost/SettingsView.swift")
(define-runtime-path macos-workflow-path
  "../macos-host/Sources/RivetHost/WorkflowEditorView.swift")
(define-runtime-path prototype-path "../design/prototype/settings.html")
(define-runtime-path design-path "../design/DESIGN.md")

;; Normalize line endings: checkouts may carry CRLF working copies.
(define (read-source path)
  (string-replace (file->string path) "\r\n" "\n"))

(define en-json (call-with-input-file en-path read-json))
(define zh-json (call-with-input-file zh-path read-json))
(define linux-source (read-source linux-path))
(define windows-settings-source (read-source windows-settings-path))
(define windows-xaml-source (read-source windows-xaml-path))
(define windows-update-source (read-source windows-update-path))
(define windows-main-source (read-source windows-main-path))
(define macos-settings-source (read-source macos-settings-path))
(define macos-workflow-source (read-source macos-workflow-path))
(define prototype-source (read-source prototype-path))
(define design-source (read-source design-path))

;; ---- one source, two complete languages ------------------------------------

(define en-keys (map symbol->string (hash-keys en-json)))
(define zh-keys (map symbol->string (hash-keys zh-json)))
(check-equal? (sort en-keys string<?) (sort zh-keys string<?)
              "shared/i18n en.json and zh.json must carry the same key set")

(define required-keys
  '("settings.nav.general" "settings.nav.features" "settings.nav.ai"
    "settings.nav.commands" "settings.nav.workflows" "settings.nav.sync"
    "settings.language.system" "settings.language.zh" "settings.language.en"
    "settings.theme.system" "settings.theme.light" "settings.theme.dark"
    "settings.provider.off" "settings.start-at-login"
    "settings.clipboard-limit" "settings.default-engine"
    "settings.plugins-timeout" "settings.ai-model" "settings.ai-base-url"
    "settings.ai-persona" "settings.ai-api-key" "settings.save"
    "settings.ai-ready" "settings.hotkey.placeholder"
    "settings.ai-key.placeholder" "settings.ai-model.placeholder"
    "settings.ai-persona.placeholder" "settings.sync-folder.placeholder"
    "settings.hint.start-at-login" "settings.hint.hotkey"
    "settings.hint.max-results" "settings.hint.clipboard-limit"
    "settings.hint.plugins-timeout" "settings.hint.expansion.mac"
    "settings.hint.expansion.linux" "settings.hint.ai-keys"
    "settings.hint.commands" "settings.hint.commands.linux"
    "settings.commands.loading" "settings.commands.load-failed"
    "settings.commands.alias" "settings.commands.press-keys"
    "settings.commands.recording" "settings.commands.record"
    "settings.commands.clear" "settings.commands.rejected"
    "settings.commands.save-failed" "settings.commands.clear-failed"
    "settings.commands.unavailable-linux" "settings.commands.saving"
    "settings.hotkey.unsupported" "settings.value.unsupported"
    "settings.write.failed" "settings.login.failed.mac"
    "settings.login.failed.win" "settings.login.failed.linux"
    "settings.ai-key.saved" "settings.ai-key.failed"
    "settings.wf.title" "settings.wf.loading" "settings.wf.hint"
    "settings.wf.new-name" "settings.wf.create" "settings.wf.refresh"
    "settings.wf.empty" "settings.wf.select" "settings.wf.page"
    "settings.wf.prev" "settings.wf.next" "settings.wf.steps"
    "settings.wf.steps-page" "settings.wf.remove" "settings.wf.add-step"
    "settings.wf.step-placeholder" "settings.wf.input"
    "settings.wf.clipboard-note" "settings.wf.dry-run"
    "settings.wf.run-copy" "settings.wf.confirm-delete" "settings.wf.delete"
    "settings.wf.trace" "settings.wf.trace-page" "settings.wf.idle"
    "settings.wf.running" "settings.wf.done" "settings.wf.enter-name"
    "settings.wf.enter-step" "settings.wf.confirm-first"
    "settings.wf.load-failed" "settings.wf.action-failed"
    "settings.wf.empty-list"
    "settings.check-updates-now" "settings.updates-note.mac"
    "settings.updates-note.win" "settings.updates-note.linux"
    "hotkeyFallbackStatus" "hotkeyTakenStatus"))
(for ([key (in-list required-keys)])
  (check-true (hash-has-key? en-json (string->symbol key))
              (format "en.json is missing ~a" key))
  (check-true (hash-has-key? zh-json (string->symbol key))
              (format "zh.json is missing ~a" key)))

;; The macOS per-action workflow notices keep their verified semantics;
;; they must stay distinct keys, not collapse into one generic "done".
(for ([suffix '("init" "load-failed" "detail-failed" "busy" "request-failed"
                "exists" "invalid-name" "created" "step-added"
                "step-unconfirmed" "bad-index" "step-removed"
                "remove-unconfirmed" "delete-unconfirmed" "deleted"
                "dry-failed" "dry-ok" "copy-only" "copied"
                "copy-unconfirmed")])
  (define key (string-append "settings.wf.notice." suffix))
  (check-true (hash-has-key? en-json (string->symbol key))
              (format "en.json is missing ~a" key)))

;; ---- six destinations and the declared row order on every host -------------

(define nav-keys
  '("settings.nav.general" "settings.nav.features" "settings.nav.ai"
    "settings.nav.commands" "settings.nav.workflows" "settings.nav.sync"))
(for ([key (in-list nav-keys)])
  (check-true (string-contains? windows-settings-source key)
              (format "Windows Settings must localize ~a" key))
  (check-true (string-contains? macos-settings-source key)
              (format "macOS Settings must localize ~a" key))
  (check-true (string-contains? linux-source key)
              (format "Linux Settings must localize ~a" key)))

;; General row order: Language → Theme → Start at login → Hotkey → Maximum
;; results. Pin the order through the construction sequence in each host.
(define (ordered? source anchors)
  (let loop ([pos 0] [anchors anchors])
    (cond
      [(null? anchors) #t]
      [else
       (define idx (string-find (substring source pos) (car anchors)))
       (and idx (loop (+ pos idx (string-length (car anchors)))
                      (cdr anchors)))])))
(define linux-build-start
  (string-find linux-source "void settings_build()"))
(define linux-build-end
  (string-find linux-source "void settings_apply("))
(define linux-build-source
  (substring linux-source linux-build-start linux-build-end))
(check-true
 (ordered? linux-build-source
           '("\"language\"" "\"theme\"" "settings.start-at-login"
             "\"hotkey\"" "\"max-results\""))
 "Linux General rows must follow Language → Theme → Start at login → Hotkey → Maximum results")
(check-true
 (ordered? windows-xaml-source
           '("LanguageBox" "ThemeBox" "FulcrumStartAtLogin" "HotkeyBox"
             "MaxResultsBox"))
 "Windows General rows must follow the declared order")

;; The language picker order is System / English / 中文 on every host.
(check-true
 (ordered? linux-source
           '("settings.language.system" "settings.language.en"
             "settings.language.zh"))
 "Linux language options must be System / English / 中文")

;; ---- settings chrome is i18n, not hardcoded English ------------------------

(check-true (string-contains? linux-source "settings.hint.commands.linux")
            "Linux must show the declared command-hotkey note")
(check-true (string-contains? linux-source "settings.hint.expansion.linux")
            "Linux must show the declared snippet-expansion note")
(check-true (string-contains? linux-source "settings.wf.enter-name")
            "Linux workflow editor strings must come from shared/i18n")
(check-true (string-contains? macos-workflow-source "settings.wf.notice.")
            "macOS workflow notices must come from shared/i18n")
(check-true (string-contains? windows-settings-source "settings.wf.")
            "Windows workflow editor strings must come from shared/i18n")
;; A language change repaints an already-open Settings window on every host.
(check-true (string-contains? linux-source "refresh_settings_language")
            "Linux must repaint the Settings window on language change")
(check-true (string-contains? windows-settings-source "RefreshLanguage")
            "Windows must repaint the Settings window on language change")
(check-true (string-contains? macos-settings-source "language")
            "macOS Settings must observe the language value")

;; ---- one dedicated update window, every entry point ------------------------

;; Windows: the inline update card is gone; tray, launcher rows and the
;; Settings Sync button all converge on UpdateWindow.
(check-true (string-contains? windows-update-source "updateCheckNow")
            "Windows UpdateWindow must offer the manual check")
(check-true (string-contains? windows-update-source "fulcrum-update-close")
            "Windows UpdateWindow must expose the close automation id")
(check-true (string-contains? windows-main-source "ShowUpdateWindow")
            "Windows MainWindow must route update entries to the window")
(check-false (string-contains? windows-main-source "StartUpdateProgressPoll")
             "the old launcher status-line update poller must stay removed")
(check-true (string-contains? windows-settings-source "SetUpdateOpener")
            "the Settings Sync button must open the dedicated update window")
;; The hide-on-close veto must not block an explicit application quit:
;; QuitApplication marks the update window closable first (the packaged
;; acceptance demands a natural process exit after 0x8006).
(check-true (string-contains? windows-main-source "update_window_->AllowClose()")
            "QuitApplication must let the update window close for real")
(check-true (string-contains? windows-update-source "allow_close_.load()")
            "the update window Closing handler must honor AllowClose")

;; Bounded shutdown: a wedged backend worker must not keep the process
;; alive after an explicit quit. A watchdog requests Application.Exit
;; after a finite grace period, and the stop thread and watchdog share a
;; once-only exit request.
(check-true (string-contains? windows-main-source "Bounded shutdown")
            "QuitApplication must bound the backend shutdown grace period")
(check-true (string-contains? windows-main-source "exit_once->exchange(true)")
            "the stop thread and watchdog must share a once-only exit request")
(check-true (string-contains? windows-main-source "sleep_for(std::chrono::seconds(10))")
            "the shutdown watchdog must fire after a finite grace period")

;; Linux: the tray check used to post into the hidden launcher's status
;; line (no visible feedback); it must open the same dedicated window.
(check-true (string-contains? linux-source "void tray_check_updates() {\n  show_update_window(true);\n}")
            "Linux tray Check for Updates must open the dedicated update window")
(check-true (string-contains? linux-source "row.id == \"update.check\"")
            "Linux launcher update rows must route to the update window")
(check-true (string-contains? linux-source "fulcrum-update-close")
            "Linux UpdateWindow must expose the close accessibility label")
(check-true (string-contains? linux-source "settings.check-updates-now")
            "Linux Settings Sync page must offer Check for Updates Now")
(check-false (string-contains? linux-source "start_update_progress_poll")
             "the old Linux status-line update poller must stay removed")

;; macOS keeps its native panel semantics (the reference implementation).
(check-true (string-contains? macos-settings-source "settings.check-updates-now")
            "macOS Settings Sync page must offer Check for Updates Now")

;; ---- design truth source ----------------------------------------------------

(for ([section '("General" "Features" "Commands" "Workflows"
                 "Sync &amp; Updates")])
  (check-true (string-contains? prototype-source section)
              (format "the settings prototype must show the ~a page" section)))
(check-true (string-contains? prototype-source "六区侧栏")
            "the settings prototype must declare the six-section sidebar")
(check-true (string-contains? design-source "六区")
            "DESIGN.md must declare the six-section settings IA")
(check-true (string-contains? design-source "176")
            "DESIGN.md must declare the 176px sidebar")
(check-true (string-contains? design-source "更新窗取消与完成态")
            "DESIGN.md must declare the cancel/downloaded platform difference")