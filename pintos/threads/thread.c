#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
static void thread_preemption (void);
void thread_update_priority (struct thread *t);
void thread_donate_priority (struct thread *t);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t; // 새 스레드 포인터
	tid_t tid; // 스레드 id 변수

	ASSERT (function != NULL); // 함수 포인터가 유효한지 검증 

	t = palloc_get_page (PAL_ZERO); // 새 스레드를 위한 페이지(4KB)를 할당

	if (t == NULL) // 할당 실패 시
		return TID_ERROR; // TID_ERROR 반환

	/* Initialize thread. */
	init_thread (t, name, priority); // 스레드 구조체 초기화 (이름, 우선순위 등 설정)
	tid = t->tid = allocate_tid (); // 고유한 스레드 ID 할당

	/* 
		스레드의 실행 컨텍스트 설정
		kernel_thread 함수가 시작점이 되도록 설정
	*/
	t->tf.rip = (uintptr_t) kernel_thread; // 시작 함수 
	t->tf.R.rdi = (uint64_t) function; // 첫 번째 인자 
	t->tf.R.rsi = (uint64_t) aux; // 두 번째 인자 
	t->tf.ds = SEL_KDSEG; // 데이터 세그먼트
	t->tf.es = SEL_KDSEG; // 추가 세그먼트
	t->tf.ss = SEL_KDSEG; // 스택 세그먼트
	t->tf.cs = SEL_KCSEG; // 코드 세그먼트
	t->tf.eflags = FLAG_IF; // 인터럽트 활성화 플래그

	/* Add to run queue. */
	thread_unblock (t); // 새 스레드를 ready_list에 추가(우선순위 순서로)
	
	thread_preemption();  // 새 스레드가 더 높은 우선순위면 즉시 스케줄링

	return tid; // 새 스레드의 id를 반환 
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 
  차단된(blocked) 스레드 T를 즉시 실행 가능한(ready-to-run) 상태로 전환합니다.
  만약 T가 차단된 상태가 아니라면 오류입니다. 
  (실행 중인 스레드를 준비 상태로 만들려면 thread_yield()를 사용하세요.)

  이 함수는 실행 중인 스레드를 선점하지 않습니다. 이는 중요할 수 있는데, 
  만약 호출자가 스스로 인터럽트를 비활성화했다면, 스레드를 원자적으로(atomically) 차단 해제하고
  다른 데이터를 업데이트할 수 있을 것으로 기대할 수 있기 때문입니다. 

  -> thread_unblock은 스레드를 준비 큐에만 추가할 뿐, 즉시 스케줄러를 호출하여 문맥교환을 발생하지 않는다!
*/

/* 
   ready_list를 우선순위 내림차순으로 유지하기 위한 비교자
   우선순위가 높을수록 앞에 오도록 정렬 (내림차순)
*/
bool thread_priority_compare(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
	// thread 구조체를 가져와야 하는 이유 : priority를 써야하므로!
	const struct thread *ta = list_entry(a, struct thread, elem); // list_elem a -> 소속 thread 구조체 주소로 역참조
	const struct thread *tb = list_entry(b, struct thread, elem); // list_elem b -> 소속 thread 구조체 주소로 역참조
	return ta->eff_priority > tb->eff_priority; // 유효 우선 순위가 클수록 먼저임
}

/* ready_list 맨 앞(최고 우선순위)이 현재보다 높으면 양보 */
static void thread_preemption(void) {
  if (intr_context()) return;                    // (1) ISR(인터럽트 핸들러) 컨텍스트에선 '즉시 스위치' 금지.
                                                 //     ※ 복귀 직후 스케줄이 필요하면 다른 경로(need_resched/intr_yield_on_return 등)에서 처리해야 함.

  enum intr_level old = intr_disable();          // (2) 짧은 임계구역 진입: 검사→결정→상태수정을 인터럽트 끼어듦 없이 원자적으로 수행
  if (!list_empty(&ready_list)) {                // (3) 대기 스레드가 하나라도 있으면
    int top = list_entry(list_front(&ready_list),
                         struct thread, elem)->eff_priority; // (4) 대기열 맨 앞(최우선) 스레드의 유효 우선순위 확인
    if (top > thread_current()->eff_priority) {      // (5) 더 높은 유효 우선순위 스레드가 준비됨 → 선점 필요
      intr_set_level(old);                       // (6) 임계구역 종료: 인터럽트 상태 복구
      thread_yield();                            // (7) 즉시 양보하여 스케줄러가 높은 우선순위를 태우게 함
      return;                                    // (8) 양보했으므로 종료
    }
  }
  intr_set_level(old);                           // (9) 선점 조건 없음 → 인터럽트 상태만 원복하고 종료
}

/* Updates thread's priority based on donations and base priority */
void thread_update_priority (struct thread *t) {
  recompute_eff_priority(t);
}

/* Ready 리스트에서 스레드를 재정렬합니다 */
void requeue_ready_list(struct thread *t) {
	ASSERT(t != NULL);
	ASSERT(t->status == THREAD_READY);
	
	enum intr_level old_level = intr_disable();
	
	/* Ready 리스트에서 제거 후 우선순위에 따라 다시 삽입 */
	list_remove(&t->elem);
	list_insert_ordered(&ready_list, &t->elem, thread_priority_compare, NULL);
	
	intr_set_level(old_level);
}


void
thread_unblock (struct thread *t) {
	enum intr_level old_level; // 인터럽트 레벨 저장용 변수 on/off를 다룸 

	/*
		인터럽트(interrupt)
		- 역할 : 인터럽트는 cpu의 상태를 제어하는 것 입니다.

		on (활성화): CPU가 외부 장치(타이머, 키보드 등)나 소프트웨어 이벤트에 의해 
		현재 작업을 중단하고 미리 정해진 인터럽트 핸들러를 실행할 수 있는 상태입니다. 
		이는 여러 작업이 동시에 진행되는 것처럼 보이게 만듭니다.

		off (비활성과): CPU가 어떤 외부 간섭에도 반응하지 않고 현재 코드만 단독으로 실행하는 상태입니다. 
		이 상태에서는 문맥 교환(context switch)이 일어나지 않으며, 타이머 틱 같은 중요한 이벤트도 처리되지 않습니다.
	*/

	ASSERT (is_thread (t)); // 유효한 스레드인지 검증

	old_level = intr_disable (); // 임계구역 시작 - 인터럽트 비활성화

	ASSERT (t->status == THREAD_BLOCKED); // 스레드가 정말 blocked 상태인지 확인

	/*
		ready_list에 우선순위 순서대로 삽입
		thread_priority_compare 함수로 우선순위 내림차순 정렬
	*/
	list_insert_ordered(&ready_list, &t->elem, thread_priority_compare, NULL);

	t->status = THREAD_READY; // 스레드 상태를 ready로 변경

	intr_set_level (old_level); // 임계구역 종료 - 인터럽트 복구
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* 현재 CPU를 양보합니다. 현재 스레드는 잠들지 않으며,
   스케줄러의 재량에 따라 즉시 다시 스케줄될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current (); // 현재 실행 중인 스레드 포인터 가져오기

	enum intr_level old_level; // 인터럽트 레벨 저장용 변수

	ASSERT (!intr_context ()); // 인터럽트 핸들러 내에서는 호출 금지 (데드락 방지)

	old_level = intr_disable (); // 임계구역 시작 - 스케줄링 도중 인터럽트 방지

	/*
		idle_thread가 아니라면 ready_list에 다시 삽입
		우선순위 순서대로 삽입하여 다음 스케줄링 때 순서 보장
	*/
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, thread_priority_compare, NULL); // 우선순위 정렬 추가

	do_schedule (THREAD_READY); // 스케줄러 호출 - 현재 스레드는 ready 상태로 변경되고 다른 스레드로 전환
	intr_set_level (old_level); // 임계구역 종료 (새로 스케줄된 스레드가 실행됨)
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정 */
void
thread_set_priority (int new_priority) {
	struct thread *cur = thread_current ();
	
	enum intr_level old = intr_disable();
	cur->base_priority = new_priority; // 기본 우선순위 업데이트
	
	/* Update effective priority considering donations */
	int before = cur->eff_priority;
	recompute_eff_priority(cur);
	if (cur->eff_priority == before) {
		intr_set_level(old);
		return;
	}
	
	/* 더 높은 우선순위 깨우는 작업 */
	if (!list_empty(&ready_list)){
		struct thread *first = list_entry(list_front(&ready_list), struct thread, elem);
		if (first->eff_priority > cur->eff_priority) {
			intr_set_level(old);
			thread_yield();
			return;
		}
	}
	intr_set_level(old);
}

/* 현재 스레드의 우선순위 반환 */
int
thread_get_priority (void) {
	return thread_current()->eff_priority; // 유효 우선순위 반환
}
/*
	예시 시나리오: 
	- main 스레드: base_priority = 31
	- acquire1이 32로 기부 -> donors 리스트에 acquire1 추가
	- acquire2이 33로 기부 -> donors 리스트에 acquire2 추가
	- thread_get_priority() 호출 시 -> 33 반환! (최고값이므로)
*/


/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->wake_ticks = 0;
	
	/* Initialize priority donation fields */
	t->base_priority = priority; // 기본 우선순위 초기값
	t->eff_priority = priority; // 유효 우선순위 초기값
	list_init (&t->donators); // 우선순위를 기부하는 스레드들의 리스트
	t->waiting_lock = NULL; // 대기 중인 락 없음
	list_init (&t->held_locks); // 보유 중인 락들의 리스트
	
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
