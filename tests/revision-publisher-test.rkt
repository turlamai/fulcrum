#lang racket/base

(require rackunit
         "../app/core/revision-publisher.rkt")

(test-case "revisions are strictly ordered under competing producers"
  (define observed '())
  (define publisher
    (make-revision-publisher
     (lambda (n)
       ;; Force a scheduler handoff while publication owns the lock.
       (sleep 0)
       (set! observed (cons n observed)))))
  (define workers
    (for/list ([_ (in-range 8)])
      (thread
       (lambda ()
         (for ([_ (in-range 50)])
           (revision-publish! publisher))))))
  (for ([worker (in-list workers)])
    (thread-wait worker))
  (check-equal? (reverse observed)
                (for/list ([n (in-range 1 401)]) n)
                "two sources must not lose, repeat or reorder revisions"))

(test-case "a failed notification cannot reset the counter or hold the lock"
  (define seen '())
  (define publisher
    (make-revision-publisher
     (lambda (n)
       (set! seen (cons n seen))
       (when (= n 1) (error 'notify "offline host")))))
  (check-exn exn:fail?
             (lambda () (revision-publish! publisher)))
  (check-equal? (revision-publish! publisher) 2)
  (check-equal? (reverse seen) '(1 2)))
