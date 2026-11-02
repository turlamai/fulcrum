#lang racket/base

;; Serialize revision assignment *and* publication. Two independent backend
;; producers (plugin results-changed and file-catalog progress) can otherwise
;; interleave a box increment with a state push, losing one increment or
;; publishing an older revision after a newer one. All platform hosts use
;; the revision to invalidate their current search results.
;;
;; The publisher is invoked under the same lock as the counter update so
;; callbacks cannot reach consumers out of order. Keep callbacks short and
;; non-reentrant; errors propagate, but the counter remains monotonic.
(provide make-revision-publisher
         revision-publish!)

(struct revision-publisher (lock counter publish))

(define (make-revision-publisher publish)
  (unless (and (procedure? publish) (procedure-arity-includes? publish 1))
    (raise-argument-error 'make-revision-publisher
                          "(procedure-arity-includes/c 1)" publish))
  (revision-publisher (make-semaphore 1) (box 0) publish))

(define (revision-publish! publisher)
  (call-with-semaphore
   (revision-publisher-lock publisher)
   (lambda ()
     (define next (add1 (unbox (revision-publisher-counter publisher))))
     (set-box! (revision-publisher-counter publisher) next)
     ((revision-publisher-publish publisher) next)
     next)))
