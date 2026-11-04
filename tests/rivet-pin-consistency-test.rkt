#lang racket/base

;; Every GitHub build must use the same immutable Rivet revision. GitHub
;; Actions does not interpolate env vars inside "uses:" action revisions, so
;; an otherwise healthy dependency bump can silently leave the native build
;; action on an older ABI than the installed Racket package.
(require racket/file
         racket/list
         racket/path
         racket/runtime-path
         racket/string
         rackunit)

(define-runtime-path repo-root "..")
(define workflows
  '("ci.yml"
    "acceptance.yml"
    "release.yml"
    "linux-appimage-upgrade-smoke.yml"
    "macos-native-update-e2e.yml"
    "windows-msi-upgrade-smoke.yml"))
(define revision-rx #px"[0-9a-f]{40}")
(define (revision-from-line line)
  (and (or (regexp-match? #rx"RIVET_PIN:" line)
           (regexp-match? #rx"rivet\\.git#" line)
           (regexp-match? #rx"setup-embed-racket@" line))
       (let ([matches (regexp-match revision-rx line)])
         (and matches (car matches)))))

(test-case "all GitHub CI, packaged acceptance, release and smoke jobs pin one Rivet revision"
  (define refs
    (for*/list ([workflow (in-list workflows)]
                [line (in-list
                       (file->lines
                        (build-path repo-root ".github" "workflows" workflow)))]
                #:do [(define revision (revision-from-line line))]
                #:when revision)
      (cons workflow revision)))
  (for ([workflow (in-list workflows)])
    (check-true (for/or ([entry (in-list refs)])
                  (string=? (car entry) workflow))
                (format "~a must pin Rivet explicitly" workflow)))
  (check-true (>= (length refs) 10)
              "every immutable action ref and package pin must be covered")
  (check-equal? (length (remove-duplicates (map cdr refs))) 1
                "an outdated action or release job must block the dependency bump"))
