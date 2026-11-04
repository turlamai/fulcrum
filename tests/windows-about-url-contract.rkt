#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string)

;; A branding/ownership migration must update the actual native About
;; action, not just README links. Keep this small source contract in CI
;; so every packaged Windows release opens a valid issue tracker.
(define-runtime-path host "../windows/MainWindow.xaml.cpp")
(define source (file->string host))

(test-case "Windows About report-issue action targets the canonical repository"
  (check-true
   (string-contains? source "https://github.com/turlamai/fulcrum/issues"))
  (check-false
   (string-contains? source "https://github.com/turinglambdaai/fulcrum/issues")))
