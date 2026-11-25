#lang racket/base

;; Fulcrum settings.
;;
;; Typed keys over `rivet/system`'s atomic JSON settings store. Values are
;; validated on read and on write; a stored value that no longer validates
;; (a hand-edited file, a downgrade) falls back to the default rather than
;; poisoning every consumer, while `settings-set!` rejects invalid writes
;; loudly because the UI must know.
;;
;; The schema is also the settings-window contract (design/DESIGN.md
;; "设置 IA"): every key names the page and section its row lives on, the
;; control a host renders and the option set a select offers. Hosts render
;; that data (`settings-ui`) instead of each keeping a hand-maintained form,
;; which is how the three native settings windows stay one feature map. A key
;; gets a row only once something honors it — no dead switches.

(require racket/contract
         racket/list
         racket/string
         "../core/ai.rkt"
         "../core/paths.rkt"
         "../core/store.rkt"
         "../core/sync.rkt"
         (prefix-in rivet: rivet/system))

(provide make-settings-manager
         settings-get
         settings-set!
         settings-list
         settings-coerce
         settings-page-groups
         settings-ui-rows
         settings-exportable-keys
         settings-spec
         settings-value->wire
         valid-hotkey?
         (struct-out settings-entry))

(struct settings-entry (key value description) #:transparent)

(struct settings-manager (store schema path) #:transparent)

;; The hotkey grammar all three hosts parse: one or more modifiers
;; (alt/ctrl/shift/cmd/win/super) then the key — space, a letter, a digit,
;; f1-f12, or one of the named punctuation keys. This is deliberately
;; narrower than what a recorded-shortcut UI would accept: every name here
;; has a concrete mapping in Carbon, Win32, and X11 code.
(define hotkey-regexp
  #px"^(?:(alt|ctrl|shift|cmd|win|super)\\+)+(space|[a-z]|[0-9]|f(?:[1-9]|1[0-2])|comma|period|minus|equal|tab)$")

(define (valid-hotkey? v)
  (and (string? v) (regexp-match? hotkey-regexp v)))

(define (one-of xs)
  (lambda (v) (and (member v xs) #t)))

(define (integer-in lo hi)
  (lambda (v) (and (exact-integer? v) (<= lo v hi))))

;; Sidebar groups and their pages, in display order (the 2026-11-20
;; prototype). `workflows` carries no schema rows: the three hosts own the
;; native workflow editor and list it after Extensions.
(define settings-page-groups
  '((workspace general files apps commands)
    (features clipboard snippets quicklinks extensions workflows windows
              calculator symbols system)
    (preferences ai privacy sync)))

;; spec = (list check default description meta)
;; meta = (list page section control options) for a settings-window row,
;;        #f for a key with no row (product internals, or owned by another
;;        surface such as the plugin center).
;; control: toggle | select | text | number | hotkey
;; options: the wire values a select offers, in display order.
(define schema
  (hasheq
   ;; ---- Workspace / General ------------------------------------------------
   'language
   (list (one-of '("system" "zh" "en")) "system"
         "UI language: system follows the OS locale (hosts resolve it); zh or en force a language"
         '(general basics select ("system" "en" "zh")))
   'theme
   (list (one-of '("system" "light" "dark")) "system"
         "UI theme: system, light, or dark"
         '(general basics select ("system" "light" "dark")))
   'hotkey
   (list valid-hotkey? "alt+space"
         "Global hotkey that summons the launcher (applied by the host): modifiers alt/ctrl/shift/cmd/win/super + key (space, a-z, 0-9, f1-f12, comma, period, minus, equal, tab)"
         '(general basics hotkey ()))
   'max-results
   (list (lambda (v) (and (exact-integer? v) (>= v 5) (<= v 12))) 12
         ;; Upper bound agrees with the panel's 12-row visual cap, lower
         ;; bound with the smallest list the panel keeps usable
         ;; (docs/design-system.md "Panel size").
         "Maximum results shown in the list"
         '(general basics number ()))
   'no-results-fallback
   (list (one-of '("web" "ai" "none")) "web"
         "What the list offers when nothing matches: a web search, an AI prompt, or nothing"
         '(general search select ("web" "ai" "none")))
   'web-search-enabled
   (list boolean? #t
         "Offer web-search rows (bangs and fallback)"
         '(general search toggle ()))
   'default-engine
   (list (one-of '("!g" "!d" "!gh" "!so" "!w" "!yt" "!m" "!t")) "!g"
         "Bang used for the generic web-search fallback row"
         '(general search select ("!g" "!d" "!gh" "!so" "!w" "!yt" "!m" "!t")))
   ;; ---- Workspace / File Search -------------------------------------------
   'files-in-launcher
   (list boolean? #t
         "Answer file queries (`find …`) from the launcher; off hides the file surface"
         '(files behavior toggle ()))
   ;; ---- Features / Clipboard ----------------------------------------------
   'clipboard-enabled
   (list boolean? #t
         "Record clipboard changes into history"
         '(clipboard history toggle ()))
   'clipboard-limit
   (list (integer-in 10 100000) 1000
         "Maximum clipboard history entries kept on device"
         '(clipboard history number ()))
   'clipboard-retention-days
   (list (one-of '(0 1 7 30 90)) 30
         "Days an unpinned clipboard entry is kept; 0 keeps them until the entry limit"
         '(clipboard history select ("1" "7" "30" "90" "0")))
   'clipboard-types
   (list (one-of '("all" "text" "links")) "all"
         "Which clipboard content is recorded: everything, text without links, or links only"
         '(clipboard history select ("all" "text" "links")))
   'clipboard-default-action
   (list (one-of '("paste" "copy")) "paste"
         "What Enter does on a clipboard row: paste into the frontmost app or copy"
         '(clipboard history select ("paste" "copy")))
   'clipboard-mask-sensitive
   (list boolean? #t
         "Mask clipboard text that looks like a credential until it is revealed explicitly"
         '(clipboard privacy toggle ()))
   ;; ---- Features / Snippets ------------------------------------------------
   'expansion-enabled
   (list boolean? #t
         "Snippet auto-expansion: typing a snippet keyword in any app replaces it with the snippet text (macOS and Windows; requires the accessibility permission on macOS)"
         '(snippets expansion toggle ()))
   ;; ---- Features / Extensions ---------------------------------------------
   'plugins-enabled
   (list boolean? #t
         "Load plugins from the plugins directory"
         '(extensions loading toggle ()))
   'plugins-timeout-ms
   (list (integer-in 100 10000) 2000
         "Per-call plugin process timeout in milliseconds"
         '(extensions loading number ()))
   'plugins-disabled
   (list string? ""
         "Comma-separated plugin ids the user turned off in the plugin center"
         #f)
   ;; ---- Features / Window Management --------------------------------------
   'window-enabled
   (list boolean? #t
         "Offer the window tiling commands (left half, right half, maximize…)"
         '(windows layout toggle ()))
   ;; ---- Features / Calculator & Units -------------------------------------
   'calc-enabled
   (list boolean? #t
         "Calculate arithmetic typed into the launcher"
         '(calculator results toggle ()))
   'calc-copy-format
   (list (one-of '("result" "expression")) "result"
         "What the calculator row copies: the result, or the expression and its result"
         '(calculator results select ("result" "expression")))
   ;; ---- Features / Emoji & Symbols ----------------------------------------
   'emoji-enabled
   (list boolean? #t
         "Offer emoji matches while typing"
         '(symbols picker toggle ()))
   'emoji-action
   (list (one-of '("paste" "copy")) "paste"
         "What Enter does on an emoji row: paste it into the frontmost app or copy it"
         '(symbols picker select ("paste" "copy")))
   ;; ---- Features / System & Scripts ---------------------------------------
   'system-enabled
   (list boolean? #t
         "Offer system commands (lock screen, sleep, empty trash…)"
         '(system commands toggle ()))
   ;; ---- Preferences / AI ---------------------------------------------------
   'ai-provider
   (list (one-of ai-providers) ""
         "BYOK AI provider: empty (off), openai, anthropic, or ollama"
         '(ai provider select ())) ; options resolved at run time (see below)
   'ai-model
   (list string? ""
         "AI model; empty uses the provider default (gpt-4o-mini / claude-3-5-haiku / llama3.2)"
         '(ai provider text ()))
   'ai-base-url
   (list (lambda (v) (or (string=? v "") (string-prefix? v "http")))
         ""
         "AI base URL; empty uses the provider default (ollama defaults to http://localhost:11434)"
         '(ai provider text ()))
   'ai-persona
   (list string? ""
         "AI persona: persistent customization prepended as a system message to every request"
         '(ai privacy text ()))
   ;; ---- Preferences / Privacy ---------------------------------------------
   'keep-history
   (list boolean? #t
         "Remember launched commands locally to rank frequent results higher"
         '(privacy data toggle ()))
   ;; ---- Preferences / Sync & Updates --------------------------------------
   'sync-root
   (list string? ""
         "Sync beta: directory a file sync service replicates (iCloud Drive, Dropbox, Syncthing); settings and snippets mirror there. Empty disables sync"
         '(sync configuration text ()))
   'update-auto-check
   (list boolean? #t
         "Check for updates quietly in the background (at most once every four hours)"
         '(sync updates toggle ()))
   ;; ---- no settings row ----------------------------------------------------
   'update-base-url
   (list (lambda (v) (and (string? v)
                          (or (string-prefix? v "https://")
                              (string-prefix? v "http://"))))
         "https://github.com/turlamai/fulcrum/releases/latest/download"
         "Base URL the updater fetches the channel manifest from"
         #f)
   'update-last-check
   (list (lambda (v) (and (exact-integer? v) (>= v 0))) 0
         "Unix epoch seconds of the last update check; the silent check runs at most once every 4 hours"
         #f)
   'update-rollout-bucket
   (list (lambda (v) (or (boolean? v)
                         (and (exact-integer? v) (>= v 0) (<= v 99))))
         #f
         "Sticky 0-99 staged-rollout bucket drawn on the first update check (false = not drawn yet)"
         #f)))

(define (spec-ref key)
  (hash-ref schema key
            (lambda ()
              (error 'settings "unknown settings key: ~a" key))))

;; The schema entry of a key: (list check default description meta).
(define/contract (settings-spec key)
  (-> symbol? (list/c procedure? any/c string? (or/c #f list?)))
  (spec-ref key))

;; The wire spelling of a typed value (what settings-list and settings-ui
;; carry and settings-coerce reads back).
(define (settings-value->wire value)
  (cond [(boolean? value) (if value "on" "off")]
        [else (format "~a" value)]))

;; The provider list is owned by ai.rkt; resolve it when a row is drawn
;; rather than copying the ids into the schema. ai-providers leads with the
;; empty id (AI off).
(define (row-options key meta)
  (if (eq? key 'ai-provider)
      ai-providers
      (list-ref meta 3)))

;; Row order inside a page, as the prototype lists them. A rowed key that is
;; missing here still renders (after the declared ones, alphabetically), and
;; the contract test fails so the order gets declared rather than drifting.
(define page-row-order
  '((general language theme hotkey max-results
             no-results-fallback web-search-enabled default-engine)
    (files files-in-launcher)
    (clipboard clipboard-enabled clipboard-limit clipboard-retention-days
               clipboard-types clipboard-default-action clipboard-mask-sensitive)
    (snippets expansion-enabled)
    (extensions plugins-enabled plugins-timeout-ms)
    (windows window-enabled)
    (calculator calc-enabled calc-copy-format)
    (symbols emoji-enabled emoji-action)
    (system system-enabled)
    (ai ai-provider ai-model ai-base-url ai-persona)
    (privacy keep-history)
    (sync sync-root update-auto-check)))

;; The sync root has to be readable before the typed manager exists (the
;; restore decision precedes loading), so peek the raw file once. A file
;; that does not parse simply means "no sync root yet".
(define (peek-sync-root path)
  (with-handlers ([exn:fail? (lambda (_) "")])
    (define raw (read-json-file path (hash)))
    (define value (if (hash? raw) (hash-ref raw 'sync-root #f) #f))
    (if (string? value) value "")))

(define/contract (make-settings-manager path #:sync-root [override #f])
  (->* (path?) (#:sync-root (or/c string? #f)) settings-manager?)
  ;; Restore from a newer mirror before the store reads the file, so a
  ;; wiped machine comes back with its settings intact. FULCRUM_SYNC_DIR
  ;; (or an explicit override) beats the stored setting: the setting lives
  ;; in the file that restore would bring back.
  (sync-restore! path (or override (sync-root-override) (peek-sync-root path)))
  (settings-manager (rivet:make-settings-store path) schema path))

(define/contract (settings-get manager key)
  (-> settings-manager? symbol? any/c)
  (define spec (spec-ref key))
  (define default (cadr spec))
  (define valid? (car spec))
  (define stored
    (with-handlers ([exn:fail? (lambda (_) default)])
      (rivet:settings-ref (settings-manager-store manager) key default)))
  (if (valid? stored) stored default))

;; Settings arrive from hosts as strings (the wire carries no booleans or
;; numbers). Coerce against the key's own validator: the literal first, then
;; the boolean words `settings-list` emits, then a number. Returns a box
;; holding the typed value or #f when nothing validates — #f alone is a
;; legitimate value.
(define/contract (settings-coerce key value)
  (-> symbol? any/c (cons/c boolean? any/c))
  (define valid? (car (spec-ref key)))
  (cond
    [(valid? value) (cons #t value)]
    [(and (string? value) (member (string-trim value) '("true" "1" "on")))
     (if (valid? #t) (cons #t #t) (cons #f #f))]
    [(and (string? value) (member (string-trim value) '("false" "0" "off")))
     (if (valid? #f) (cons #t #f) (cons #f #f))]
    [(and (string? value) (string->number (string-trim value)))
     => (lambda (n) (if (valid? n) (cons #t n) (cons #f #f)))]
    [else (cons #f #f)]))

(define/contract (settings-set! manager key value)
  (-> settings-manager? symbol? any/c boolean?)
  (define spec (spec-ref key))
  (define valid? (car spec))
  (unless (valid? value)
    (raise-argument-error 'settings-set!
                          (format "valid value for ~a" key) value))
  (rivet:settings-set! (settings-manager-store manager) key value)
  ;; Mirror the whole file after every successful write. Reading the root
  ;; back through the typed manager also means a mid-session sync-root
  ;; change takes effect from the very next write.
  (sync-mirror! (settings-manager-path manager)
                (or (sync-root-override) (settings-get manager 'sync-root)))
  #t)

(define/contract (settings-list manager)
  (-> settings-manager? (listof settings-entry?))
  (for/list ([key (in-list (sort (hash-keys (settings-manager-schema manager)) symbol<?))])
    (define spec (hash-ref (settings-manager-schema manager) key))
    (define value (settings-get manager key))
    (settings-entry (symbol->string key)
                    ;; Booleans travel as on/off — the words the launcher's
                    ;; setting badges already use and settings-set accepts
                    ;; back — so hosts never meet Racket's #t/#f spelling.
                    (settings-value->wire value)
                    (caddr spec))))

;; ---- settings-window data -------------------------------------------------

;; The rows of every settings page, in page then section then schema order.
;; A section heading is `('section page section)`; a row is
;; `('setting key page section control options value)` with options and
;; value already in wire form. Pages without schema rows (the list pages
;; and the workflow editor) simply emit nothing.
(define/contract (settings-ui-rows manager)
  (-> settings-manager? (listof list?))
  (define by-page
    (for/hash ([key (in-list (hash-keys (settings-manager-schema manager)))])
      (define meta (list-ref (hash-ref (settings-manager-schema manager) key) 3))
      (values key meta)))
  (define (row-rank page key)
    (define declared (or (assq page page-row-order) (cons page '())))
    (define position (index-of (cdr declared) key))
    (if position position (+ (length (cdr declared)) 1000)))
  (append*
   (for/list ([page (in-list (apply append (map cdr settings-page-groups)))])
     (define rows
       (sort (for/list ([(key meta) (in-hash by-page)]
                       #:when (and meta (eq? (list-ref meta 0) page)))
               (cons key meta))
             ;; Declared row order; keys without a declared position follow.
             (lambda (a b)
               (define ra (row-rank page (car a)))
               (define rb (row-rank page (car b)))
               (if (= ra rb)
                   (string<? (symbol->string (car a)) (symbol->string (car b)))
                   (< ra rb)))))
     (define sections
       (remove-duplicates (map (lambda (r) (list-ref (cdr r) 1)) rows)))
     (append*
      (for/list ([section (in-list sections)])
        (cons (list 'section page section)
              (for/list ([r (in-list rows)]
                         #:when (eq? (list-ref (cdr r) 1) section))
                (define key (car r))
                (define meta (cdr r))
                (list 'setting key page section (list-ref meta 2)
                      (row-options key meta)
                      (settings-value->wire (settings-get manager key))))))))))

;; Keys an export may carry, by prototype category. Appearance is the
;; General page minus the hotkey (a device binding); Features is every
;; toggle and select on a Features page — never a free-text field, which can
;; hold personal content. AI, Privacy, Sync, credentials, source paths,
;; indexes, history, permission grants and startup never leave the device.
(define/contract (settings-exportable-keys category)
  (-> (or/c 'appearance 'features) (listof symbol?))
  (define feature-pages (cdr (assq 'features settings-page-groups)))
  (for/list ([key (in-list (sort (hash-keys schema) symbol<?))]
             #:when (let ([meta (list-ref (hash-ref schema key) 3)])
                      (and meta
                           (memq (list-ref meta 2) '(toggle select))
                           (case category
                             [(appearance)
                              (and (eq? (list-ref meta 0) 'general)
                                   (not (eq? key 'hotkey)))]
                             [(features) (memq (list-ref meta 0) feature-pages)]
                             [else #f]))))
    key))
