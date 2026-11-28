#lang racket

;; Settings v2 (#241): one schema drives the preferences, the settings
;; window rows and the i18n keys the three hosts resolve; the named lists
;; the settings pages edit; JSON import / export; and the engine and
;; clipboard honoring the preferences they own.

(require json
         racket/match
         racket/file
         racket/list
         racket/port
         rackunit
         (only-in racket/runtime-path define-runtime-path)
         "../app/core/apps.rkt"
         "../app/core/bindings.rkt"
         "../app/core/clipboard.rkt"
         "../app/core/engine.rkt"
         "../app/core/lists.rkt"
         "../app/core/paths.rkt"
         "../app/core/quicklinks.rkt"
         "../app/core/settings.rkt"
         "../app/core/settings-transfer.rkt"
         "../app/core/snippets.rkt")

(define (call-with-dir thunk)
  (define dir (make-temporary-file "fulcrum-s2test-~a" 'directory))
  (dynamic-wind
    void
    (lambda () (thunk dir))
    (lambda ()
      (with-handlers ([exn:fail? void]) (delete-directory/files dir)))))

;; A throwaway data directory for the engine: the recents file and the
;; stores it opens by default must never touch the developer's own profile.
(define (call-with-profile thunk)
  (define dir (make-temporary-file "fulcrum-s2prof-~a" 'directory))
  (parameterize ([current-environment-variables
                  (let ([env (current-environment-variables)])
                    (environment-variables-set! env #"FULCRUM_DATA_DIR"
                                                (string->bytes/utf-8 (path->string dir)))
                    env)])
    (dynamic-wind
      void
      thunk
      (lambda ()
        (with-handlers ([exn:fail? void]) (delete-directory/files dir))))))

(define-runtime-path en-path "../shared/i18n/en.json")
(define-runtime-path zh-path "../shared/i18n/zh.json")

(define (i18n-table lang)
  (call-with-input-file (if (eq? lang 'en) en-path zh-path) read-json))

(define (row-id row) (list-ref row 0))
(define (row-kind row) (list-ref row 3))

;; i18n keys a settings-ui row resolves on a host: labels, hints, page and
;; section names, and the display name of every select option.
(define (i18n-keys-for row)
  (match row
    [(list 'page page group)
     (list (format "settings.nav.~a" page) (format "settings.page.~a" page)
           (format "settings.group.~a" group))]
    [(list 'section page section)
     (list (format "settings.nav.~a" page) (format "settings.section.~a" section))]
    [(list 'setting key _page section 'select options _value)
     (append (list (format "settings.~a" key)
                   (format "settings.hint.~a" key)
                   (format "settings.section.~a" section))
             (for/list ([option (in-list options)])
               ;; an empty option value (AI provider off) is named "off"
               (format "settings.opt.~a.~a" key (if (equal? option "") 'off option))))]
    ;; A number row's options are its bounds; no display name to resolve.
    [(list 'setting key _page section _control _options _value)
     (list (format "settings.~a" key)
           (format "settings.hint.~a" key)
           (format "settings.section.~a" section))]
    [_ '()]))

;; ---- schema ----------------------------------------------------------------

(test-case "the new preferences have typed defaults"
  (call-with-dir
   (lambda (dir)
     (define s (make-settings-manager (build-path dir "settings.json")))
     (for ([key (in-list '(files-in-launcher clipboard-mask-sensitive window-enabled
                              calc-enabled emoji-enabled system-enabled keep-history
                              update-auto-check))])
       (check-true (settings-get s key) (format "~a defaults on" key)))
     (check-equal? (settings-get s 'no-results-fallback) "web")
     (check-equal? (settings-get s 'clipboard-retention-days) 30)
     (check-equal? (settings-get s 'clipboard-types) "all")
     (check-equal? (settings-get s 'clipboard-default-action) "paste")
     (check-equal? (settings-get s 'calc-copy-format) "result")
     (check-equal? (settings-get s 'emoji-action) "paste"))))

(test-case "coercion follows the key's own validator"
  ;; The literal first, then the on/off words the wire carries, then a number.
  (check-equal? (settings-coerce 'theme "dark") '(#t . "dark"))
  (check-equal? (settings-coerce 'clipboard-enabled "off") '(#t . #f))
  (check-equal? (settings-coerce 'clipboard-retention-days "7") '(#t . 7))
  (check-equal? (settings-coerce 'max-results "9") '(#t . 9))
  ;; Out of range and outside the option set are refused; #f itself is a
  ;; legal value, so refusal is the pair's flag, never the value.
  (check-false (car (settings-coerce 'max-results "99")))
  (check-false (car (settings-coerce 'no-results-fallback "bing")))
  (check-false (car (settings-coerce 'clipboard-enabled "maybe"))))

(test-case "settings-ui rows follow the declared page, section and row order"
  (call-with-dir
   (lambda (dir)
     (define s (make-settings-manager (build-path dir "settings.json")))
     (define rows (settings-ui-rows s))
     (define general
       (for/list ([row (in-list rows)]
                  #:when (and (eq? (car row) 'setting) (eq? (list-ref row 2) 'general)))
         (list-ref row 1)))
     (check-equal? general '(language theme hotkey max-results
                             no-results-fallback web-search-enabled default-engine))
     ;; The list opens with the pages in sidebar order, then each page's
     ;; sections and rows; a section heading precedes the rows it names.
     (check-equal? (take rows 2) '((page general workspace) (section general basics)))
     (check-equal? (list-ref (list-ref rows 1) 2) 'basics)
     ;; A number row carries its inclusive bounds where a select has options.
     (define max-results
       (findf (lambda (r) (and (eq? (car r) 'setting) (eq? (list-ref r 1) 'max-results)))
              rows))
     (check-equal? (list-ref max-results 4) 'number)
     (check-equal? (list-ref max-results 5) '(5 12))
     ;; Options travel in display order.
     (define fallback
       (findf (lambda (r) (and (eq? (car r) 'setting)
                               (eq? (list-ref r 1) 'no-results-fallback)))
              rows))
     (check-equal? (list-ref fallback 5) '("web" "ai" "none"))
     (check-equal? (list-ref fallback 6) "web"))))

(test-case "every settings-ui row has its labels in both languages"
  ;; The hosts resolve settings.<key>, settings.hint.<key>,
  ;; settings.nav.<page>, settings.section.<section> and
  ;; settings.opt.<key>.<option>. A key without them would render as a raw
  ;; key on a host, so the gap fails here instead.
  (call-with-dir
   (lambda (dir)
     (define rows (settings-ui-rows (make-settings-manager
                                     (build-path dir "settings.json"))))
     (for ([lang (in-list '(en zh))])
       (define table (i18n-table lang))
       (for* ([row (in-list rows)])
         (for ([key (in-list (i18n-keys-for row))])
           (check-true (hash-has-key? table (string->symbol key))
                       (format "~a lacks ~a" lang key))))))))

(test-case "exportable keys never include a free-text field or a device binding"
  (for ([category (in-list '(appearance features))])
    (for ([key (in-list (settings-exportable-keys category))])
      (define control (list-ref (list-ref (settings-spec key) 3) 2))
      (check-not-false (memq control '(toggle select))
                       (format "~a ~a is a ~a" category key control))
      (check-false (eq? key 'hotkey))))
  (check-not-false (memq 'theme (settings-exportable-keys 'appearance)))
  (check-not-false (memq 'calc-enabled (settings-exportable-keys 'features)))
  (check-false (memq 'ai-persona (settings-exportable-keys 'features)))
  (check-false (memq 'sync-root (settings-exportable-keys 'appearance))))

;; ---- named lists -------------------------------------------------------------

(test-case "named lists validate, persist and refuse duplicates"
  (call-with-dir
   (lambda (dir)
     (define path (build-path dir "lists.json"))
     (define store (make-list-store path))
     ;; A folder kind needs a path; an ignore rule needs a pattern.
     (check-exn #px"^path-required"
                (lambda () (list-set! store "app-folders" "" "Tools" "" "" #t)))
     (check-exn #px"^pattern-required"
                (lambda () (list-set! store "ignore-rules" "" "node modules" "" "" #t)))
     (check-exn #px"^unknown-kind"
                (lambda () (list-items store "bookmarks")))
     (define folder
       (list-set! store "app-folders" "" "Tools" "/opt/tools" "" #t))
     (check-true (string? (list-item-id folder)))
     ;; The same name twice would be a no-op the user never notices.
     (check-exn #px"^duplicate-name"
                (lambda () (list-set! store "app-folders" "" "tools" "/elsewhere" "" #t)))
     ;; Editing keeps the id and position; the enabled flag is the switch.
     (define edited
       (list-set! store "app-folders" (list-item-id folder) "Tools" "/opt/tools2" "" #f))
     (check-equal? (list-item-id edited) (list-item-id folder))
     (check-false (list-item-enabled edited))
     ;; Ignore rules may share a name; each is its own pattern.
     (list-set! store "ignore-rules" "" "cache" "" "node_modules/**" #t)
     (list-set! store "ignore-rules" "" "cache" "" ".cache/**" #t)
     (check-equal? (length (list-items store "ignore-rules")) 2)
     ;; Reload from disk: the file is the truth.
     (define again (make-list-store path))
     (check-equal? (map list-item-path (list-items again "app-folders"))
                   '("/opt/tools2"))
     (check-true (list-remove! again "app-folders" (list-item-id folder)))
     (check-false (list-remove! again "app-folders" (list-item-id folder)))
     (check-equal? (list-items (make-list-store path) "app-folders") '()))))

(test-case "discovery roots include the application folders that exist"
  (call-with-dir
   (lambda (dir)
     (define folder (build-path dir "Extra Apps"))
     (make-directory folder)
     (parameterize ([extra-discovery-roots
                     (list (path->string folder)
                           (path->string (build-path dir "missing")))])
       (check-not-false (member (path->string folder)
                               (map (lambda (p) (if (path? p) (path->string p) p))
                                    (discovery-roots))))))))

;; ---- engine honors preferences -----------------------------------------------

(define (fixture-engine prefs #:apps [apps '()] #:hidden [hidden '()]
                         #:clipboard [clipboard #f])
  (define engine
    (make-engine #:clipboard-store clipboard
                 #:snippet-store #f
                 #:plugin-manager #f
                 #:pref-thunk (lambda (key) (hash-ref prefs key 'unset))
                 #:hidden-apps-thunk (lambda () hidden)))
  (define index (make-hash))
  (for ([app (in-list apps)])
    (hash-set! index (application-id app) app))
  (set-engine-apps-index! engine index)
  engine)

(define two-apps
  (list (application "a1" "Firefox" "/usr/bin/firefox" "app" '() "")
        (application "a2" "Terminal" "/usr/bin/terminal" "app" '() "")))

(test-case "a switched-off surface answers nothing"
  (call-with-profile
   (lambda ()
     (define on (fixture-engine (hash)))
     (check-true (pair? (engine-search on "12*12")))
     (define off (fixture-engine (hash 'calc-enabled #f)))
     (check-false (member "Calculator" (map row-kind (engine-search off "12*12"))))
     (define quiet (fixture-engine (hash 'system-enabled #f)))
     (check-false (member "System" (map row-kind (engine-search quiet "lock")))))))

(test-case "the calculator copies what the settings page says"
  (call-with-profile
   (lambda ()
     (define row (car (engine-search (fixture-engine (hash)) "12*12")))
     (check-equal? (list-ref row 4) "144")
     (define expression (fixture-engine (hash 'calc-copy-format 'expression)))
     (check-equal? (list-ref (car (engine-search expression "12*12")) 4) "12*12 = 144"))))

(test-case "emoji follow the configured Enter action"
  (call-with-profile
   (lambda ()
     (define (emoji-row engine)
       (findf (lambda (r) (string=? (row-id r) "emoji.paste"))
              (engine-search engine "smile")))
     (define row (emoji-row (fixture-engine (hash))))
     (check-not-false row)
     (define events
       (cdr (engine-run (fixture-engine (hash)) "emoji.paste" (list-ref row 4))))
     (check-equal? (car (car events)) 'paste-to-frontmost)
     (define copied
       (cdr (engine-run (fixture-engine (hash 'emoji-action 'copy))
                        "emoji.paste" (list-ref row 4))))
     (check-equal? (car (car copied)) 'copy-to-clipboard))))

(define (app-titles engine query)
  (for/list ([r (in-list (engine-search engine query))]
             #:when (string=? (row-kind r) "Application"))
    (list-ref r 1)))

(test-case "hidden applications drop out of results without a rebuild"
  (call-with-profile
   (lambda ()
     (define shown (fixture-engine (hash) #:apps two-apps))
     (check-equal? (app-titles shown "firefox") '("Firefox"))
     ;; Hidden names match case-insensitively, and nothing else moves.
     (define hidden (fixture-engine (hash) #:apps two-apps #:hidden '("FIREFOX")))
     (check-equal? (app-titles hidden "firefox") '())
     (check-equal? (app-titles hidden "terminal") '("Terminal")))))

(test-case "clipboard default action and masking follow the settings page"
  (call-with-dir
   (lambda (dir)
     (define store (make-clipboard-store (build-path dir "clip.json")))
     (clipboard-record! store "hello world")
     (define engine
       (fixture-engine (hash) #:clipboard store))
     (define row (car (engine-search engine "hello")))
     (check-equal? (row-id row) "clip.paste")
     ;; The action is read at run time: the same store, the setting flipped.
     (define copy
       (make-engine #:clipboard-store store #:snippet-store #f #:plugin-manager #f
                    #:pref-thunk (lambda (key)
                                   (if (eq? key 'clipboard-default-action) 'copy 'unset))))
     (define events (cdr (engine-run copy "clip.paste" (list-ref row 4))))
     (check-equal? (car (car events)) 'copy-to-clipboard)
     ;; The secondary action is the other one, so both stay reachable.
     (define actions (engine-row-actions copy "clip.paste" (list-ref row 4)))
     (check-not-false (member "Paste to frontmost"
                             (map (lambda (r) (list-ref r 1)) actions)))
     ;; The default engine still pastes.
     (define pasted (cdr (engine-run engine "clip.paste" (list-ref row 4))))
     (check-equal? (car (car pasted)) 'paste-to-frontmost))))

(test-case "clipboard retention and content classes are honored"
  (call-with-dir
   (lambda (dir)
     (define path (build-path dir "clip.json"))
     (define store (make-clipboard-store path))
     (check-not-false (clipboard-record! store "https://example.com/doc" #:types 'links))
     (check-false (clipboard-record! store "plain text" #:types 'links))
     (check-false (clipboard-record! store "https://example.com/x" #:types 'text))
     (clipboard-record! store "https://example.com/y" #:types 'links)
     (check-equal? (clipboard-count store) 2)
     ;; Age both entries past a 30-day window on disk; loading with that
     ;; window expires them, loading without a window keeps them.
     (define aged
       (for/list ([entry (in-list (with-input-from-file path read-json))])
         (hash-set entry 'ts (- (current-inexact-milliseconds)
                                (* 40 24 3600 1000)))))
     (with-output-to-file path #:exists 'truncate
       (lambda () (write-json aged)))
     (check-equal?
      (clipboard-count (make-clipboard-store path #:retention-days 30)) 0)
     (with-output-to-file path #:exists 'truncate
       (lambda () (write-json aged)))
     (check-equal? (clipboard-count (make-clipboard-store path)) 2))))

(test-case "launch history can be switched off and cleared"
  (call-with-profile
   (lambda ()
     (define keep (fixture-engine (hash)))
     (engine-record-use! keep "sys.run" "Lock Screen")
     (check-true (> (hash-count (engine-recents keep)) 0))
     (define off (fixture-engine (hash 'keep-history #f)))
     (engine-record-use! off "sys.run" "Lock Screen")
     (check-equal? (hash-count (engine-recents off)) 0)
     (engine-clear-history! keep)
     (check-equal? (hash-count (engine-recents keep)) 0))))

;; ---- import / export ---------------------------------------------------------

(define (fresh-stores dir)
  (values (make-settings-manager (build-path dir "settings.json"))
          (make-bindings-store (build-path dir "bindings.json"))
          (make-snippet-store (build-path dir "snippets.json"))
          (make-quicklink-store (build-path dir "quicklinks.json"))))

(test-case "export → import round-trips appearance, features, commands and content"
  (call-with-dir
   (lambda (dir)
     (define-values (s b snips links) (fresh-stores dir))
     (settings-set! s 'theme "dark")
     (settings-set! s 'calc-enabled #f)
     (binding-set! b "win.left" "lh" #f)
     (snippet-save! snips "Signature" "— Ji" "sig")
     (link-save! links "Docs" "https://example.com/docs" "docs")
     (define payload
       (settings-export-json s b snips links '(appearance features commands content)))
     (define data (with-input-from-string payload read-json))
     (check-equal? (hash-ref data 'format) "fulcrum-settings")
     (check-equal? (hash-ref data 'version) 2)
     (define groups (hash-ref data 'groups))
     ;; Never exported: the hotkey (a device binding), AI, sync, paths.
     (check-false (hash-has-key? (hash-ref groups 'appearance) 'hotkey))
     (check-false (hash-has-key? (hash-ref groups 'appearance) 'sync-root))
     (check-false (hash-has-key? groups 'ai))
     (check-equal? (hash-ref (hash-ref groups 'appearance) 'theme) "dark")
     (check-equal? (hash-ref (hash-ref groups 'features) 'calc-enabled) #f)
     (check-equal? (length (hash-ref groups 'commands)) 1)
     ;; Import into a clean profile: validate first, then apply.
     (call-with-dir
      (lambda (dir2)
        (define-values (s2 b2 snips2 links2) (fresh-stores dir2))
        (define plan (settings-import-plan s2 payload))
        (check-not-false (regexp-match? #rx"features:[0-9]+" (plan-summary plan)))
        (transfer-apply! s2 b2 snips2 links2 plan void)
        (check-equal? (settings-get s2 'theme) "dark")
        (check-false (settings-get s2 'calc-enabled))
        (check-equal? (binding-alias-for b2 "win.left") "lh")
        (check-equal? (length (snippet-list snips2)) 1)
        (check-equal? (length (link-list links2)) 1))))))

(test-case "import replaces the selected categories wholesale"
  (call-with-dir
   (lambda (dir)
     (define-values (s b snips links) (fresh-stores dir))
     (snippet-save! snips "Old" "old text" "old")
     (snippet-save! snips "Keep" "keep text" "keep")
     (define payload
       (jsexpr->string
        (hasheq 'format "fulcrum-settings" 'version 2
                'groups (hasheq 'content (hasheq 'snippets '() 'quicklinks '())))))
     (transfer-apply! s b snips links (settings-import-plan s payload) void)
     (check-equal? (snippet-list snips) '()))))

(test-case "import refuses malformed, unknown and conflicting files"
  (call-with-dir
   (lambda (dir)
     (define-values (s b _snips _links) (fresh-stores dir))
     (define (reject payload code)
       (check-exn (regexp (format "^~a" code))
                  (lambda () (settings-import-plan s payload))
                  payload))
     (define (file groups)
       (jsexpr->string (hasheq 'format "fulcrum-settings" 'version 2 'groups groups)))
     (reject "not json" "format")
     (reject (jsexpr->string (hasheq 'format "fulcrum-settings" 'version 1
                                     'groups (hasheq)))
             "format")
     ;; A key outside the export set — even one the product has.
     (reject (file (hasheq 'appearance (hasheq 'hotkey "ctrl+x"))) "value")
     (reject (file (hasheq 'appearance (hasheq 'theme "neon"))) "value")
     (reject (file (hasheq 'appearance (hasheq 'max-results 99))) "value")
     ;; The same hotkey twice inside the file; and the summon hotkey.
     (reject (file (hasheq 'commands
                           (list (hasheq 'id "win.left" 'alias "lh" 'hotkey "ctrl+shift+1")
                                 (hasheq 'id "win.right" 'alias "rh"
                                         'hotkey "ctrl+shift+1"))))
             "conflict")
     (reject (file (hasheq 'commands
                           (list (hasheq 'id "win.left" 'alias "" 'hotkey "alt+space"))))
             "conflict")
     (reject (file (hasheq 'bookmarks (hasheq))) "empty"))))
