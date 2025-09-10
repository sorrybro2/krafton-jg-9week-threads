/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_push_back (&sema->waiters, &thread_current ()->elem);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	struct thread *next = NULL;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		/* Sort waiters by priority before unblocking highest priority thread */
		list_sort (&sema->waiters, thread_priority_compare, NULL);
		next = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
		thread_unblock (next);
	}
	sema->value++;
	intr_set_level (old_level);
	
	/* 깨운 스레드가 현재 스레드보다 높은 우선순위면 양보 */
	if (next && next->eff_priority > thread_current()->eff_priority) {
		if (intr_context()) 
			intr_yield_on_return();
		else 
			thread_yield();
	}
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}



/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	enum intr_level old = intr_disable(); // 인터럽트 
	struct thread *cur = thread_current (); // 현재 스레드
	
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	/* 만약 락이 이미 다른 스레드에 의해 잡혀있다면 */
	if (lock->holder != NULL) {
		cur->waiting_lock = lock; // "나는 이 락을 기다리고 있다" 표시
		/* Add current thread to lock holder's donators list */
		list_insert_ordered (&lock->holder->donators, &cur->donate_elem,
		                     higher_priority_donate, NULL);
		/* Donate priority through the chain */
		propagate_eff_priority();
	}

	sema_down (&lock->semaphore); // 락을 얻을 때까지 대기 (블럭됨)
	
	/* 락을 얻었다! 이제 더 이상 기다리지 않음 */
	cur->waiting_lock = NULL; // 기다리는 락 없음
	lock->holder = cur; // 내가 락의 새로운 소유자
	list_push_back(&cur->held_locks, &lock->elem); // 보유 락 리스트에 추가

	intr_set_level(old);
}

/*
	전파 동안 해야할 일
	1. lock_donators 재정렬 : Cuz cur의 유효 우선순위가 바꼈으니 
	2. 새로운 유효 우선순위 계산 및 검증 
	3. 변경 됐고 ready상태라면 ready_list update
*/
void propagate_eff_priority() {
	enum intr_level old = intr_disable();
	struct thread *cur = thread_current();
	struct lock *w_lock = cur->waiting_lock;
	int depth = 0;
	const int MAX_DEPTH = 8; // 무한 루프 방지
	
 	while ((w_lock = cur->waiting_lock) && w_lock->holder && depth < MAX_DEPTH)
	{
		depth++;
		struct thread *w_lock_holder = w_lock->holder;

		// 1. lock_donators 재정렬
		bool is_donator = is_contain(&w_lock_holder->donators, &cur->donate_elem);
		if (!list_empty(&w_lock_holder->donators) && is_donator){
 			list_remove(&cur->donate_elem);
		}
 		list_insert_ordered(&w_lock_holder->donators, &cur->donate_elem, 
		higher_priority_donate, NULL);

		int before_eff = w_lock_holder->eff_priority;
		if (cur->eff_priority <= before_eff) break;

		 // 2. 새로운 유효 우선순위 계산
		recompute_eff_priority(w_lock_holder);

		// 변화 없으면 기부 안한걸로 치고 그 뒤 전파도 필요 없음
		if (w_lock_holder->eff_priority == before_eff) break; 
		if (w_lock_holder->status == THREAD_READY) {
			requeue_ready_list(w_lock_holder);
		}
		cur = w_lock_holder;
	}
	
	intr_set_level(old);
}


/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	
	enum intr_level old = intr_disable();
	struct thread *cur = lock->holder;
	int before_eff_priority = cur->eff_priority;
	
	/* 1. 해당 Lock과 관련된 donators 제거 */
	if (!list_empty(&cur->donators)){
		remove_donators_related_lock(cur, lock);
		/* 2. 유효 우선순위 재계산 */
		recompute_eff_priority(cur);
	}
	
	/* 3. 더 낮아졌고, READY상태라면 => ready_list 갱신 */
	if ((before_eff_priority != cur->eff_priority) &&
		cur->status == THREAD_READY) {
		requeue_ready_list(cur);
	}
	
	/* 4. held_locks 리스트에서 제거 */
	list_remove(&lock->elem);
	
	/* 5. 본격적인 Lock 해제 작업 */
	lock->holder = NULL;
	sema_up (&lock->semaphore); // 여기서 어차피 unblock함
	intr_set_level(old);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		/* Sort waiters by priority before signaling highest priority thread */
		list_sort (&cond->waiters, cond_sema_priority_compare, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

/* ========== Priority Donation Helper Functions ========== */

/* 스레드의 유효 우선순위를 재계산합니다 */
void recompute_eff_priority(struct thread *t) {
	ASSERT(t != NULL);
	
	int max_priority = t->base_priority; // 기본 우선순위부터 시작
	
	/* 기부자들 중 최고 우선순위 찾기 */
	if (!list_empty(&t->donators)) {
		struct list_elem *e;
		for (e = list_begin(&t->donators); e != list_end(&t->donators); e = list_next(e)) {
			struct thread *donator = list_entry(e, struct thread, donate_elem);
			if (donator->eff_priority > max_priority) {
				max_priority = donator->eff_priority;
			}
		}
	}
	
	t->eff_priority = max_priority;
}

/* Ready 리스트에서 스레드를 재정렬합니다 - thread.c에서 구현됨 */

/* 우선순위 비교 함수 (기부자 리스트용) */
bool higher_priority_donate(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *ta = list_entry(a, struct thread, donate_elem);
	struct thread *tb = list_entry(b, struct thread, donate_elem);
	return ta->eff_priority > tb->eff_priority;
}

/* 특정 락과 관련된 기부자들을 제거합니다 */
void remove_donators_related_lock(struct thread *t, struct lock *lock) {
	ASSERT(t != NULL);
	ASSERT(lock != NULL);
	
	struct list_elem *e = list_begin(&t->donators);
	
	while (e != list_end(&t->donators)) {
		struct thread *donator = list_entry(e, struct thread, donate_elem);
		struct list_elem *next = list_next(e);
		
		if (donator->waiting_lock == lock) {
			list_remove(e);
		}
		e = next;
	}
}

/* 리스트에 특정 요소가 포함되어 있는지 확인 */
bool is_contain(struct list *list, struct list_elem *elem) {
	ASSERT(list != NULL);
	ASSERT(elem != NULL);
	
	struct list_elem *e;
	for (e = list_begin(list); e != list_end(list); e = list_next(e)) {
		if (e == elem) {
			return true;
		}
	}
	return false;
}

/* Condition variable의 semaphore_elem 우선순위 비교 함수 */
bool cond_sema_priority_compare(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);
	
	/* semaphore의 waiters에서 가장 높은 우선순위를 가진 스레드 찾기 */
	struct thread *ta = NULL;
	struct thread *tb = NULL;
	
	if (!list_empty(&sa->semaphore.waiters)) {
		ta = list_entry(list_front(&sa->semaphore.waiters), struct thread, elem);
	}
	if (!list_empty(&sb->semaphore.waiters)) {
		tb = list_entry(list_front(&sb->semaphore.waiters), struct thread, elem);
	}
	
	if (ta == NULL && tb == NULL) return false;
	if (ta == NULL) return false;
	if (tb == NULL) return true;
	
	return ta->eff_priority > tb->eff_priority;
}
