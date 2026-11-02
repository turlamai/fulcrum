#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; A second Windows launch must not silently disappear when a previous
;; resident process holds the lease but no longer has a responsive window.
;; Real package acceptance covers the working-primary summon path; this
;; source contract guards the otherwise hard-to-stage stale-owner branch.
(define-runtime-path source-path "../windows/App.xaml.cpp")
(define source (file->string source-path))

(check-true (string-contains? source "SendMessageTimeoutW")
            "summon delivery must be acknowledged or time out")
(check-true (string-contains? source "SMTO_ABORTIFHUNG")
            "a hung resident must never freeze the second launcher")
(check-true (string-contains? source "instance_lease_.reset();")
            "reacquire the lease after the old process releases it")
(check-true (string-contains? source "Fulcrum could not start")
            "report inaccessible owner rather than silently exit")
(check-false (string-contains? source "TerminateProcess(")
             "never force-kill the old or unrelated application")
