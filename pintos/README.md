# 🎯 Pintos Priority Scheduling 완벽 구현 가이드

## 🚀 Priority Donation이란?

**우선순위 역전 문제**를 해결하는 핵심 메커니즘입니다.

```
문제: 높은 우선순위 스레드 → 락 대기 → 낮은 우선순위 스레드가 락 보유
결과: 중간 우선순위 스레드들이 먼저 실행되는 역전 현상 😱

해결: 높은 우선순위가 낮은 우선순위에게 "기부" → 빠른 처리 → 락 획득 🎯
```

## 📊 전체 실행 흐름

> 첨부된 시퀀스 다이어그램이 여기에 들어갑니다

**핵심 단계:**
1. **Main(31)이 Lock1 획득**
2. **ThreadA(32) 대기** → Main에게 우선순위 32 기부 → `eff_priority: 31→32`
3. **ThreadB(33) 대기** → Main에게 우선순위 33 기부 → `eff_priority: 32→33`
4. **Main이 Lock1 해제** → donators 제거 → `eff_priority: 33→31` 복구
5. **ThreadB(33) 먼저 실행** → ThreadA(32) 나중 실행 (우선순위 순서)

---

## 🔒 뮤텍스 vs 세마포어 완벽 구분

### **🔒 뮤텍스(Lock): "임계구간을 한 스레드씩 독점"**

**특징:**
- ✅ **소유권 존재** (acquire한 스레드만 release 가능)
- ✅ **Priority Donation** (높은 → 낮은 우선순위로 기부)
- ✅ **상호배제** (한 번에 하나만 임계구간 진입)

**사용 예시:**
```c
struct lock file_lock;
int shared_data = 0;

void critical_work() {
    lock_acquire(&file_lock);    // 🔒 "나만 들어간다!"
    shared_data++;               // 임계구간: 안전한 접근
    lock_release(&file_lock);    // 🔓 "다음 사람 들어와!"
}
```

### **🔗 세마포어: "우선순위로 양보하고 순서 결정"**

**특징:**
- ✅ **소유권 없음** (누구나 up/down 가능)
- ✅ **우선순위 스케줄링** (높은 우선순위부터 깨우기)
- ✅ **이벤트 동기화** (완료 신호, 순서 제어)

**사용 예시:**
```c
struct semaphore task_signal;

void high_priority_work() {
    sema_down(&task_signal);     // 🏃‍♂️ "우선순위 높으니까 먼저!"
}

void low_priority_work() {
    sema_down(&task_signal);     // 😴 "높은 우선순위 끝날 때까지 대기..."
}

void coordinator() {
    sema_up(&task_signal);       // 📢 "시작해도 돼!" → 높은 우선순위부터 깨어남
}
```

### **🎯 언제 뭘 써야 할까?**

| 상황 | 사용할 것 | 이유 |
|------|-----------|------|
| 공유 데이터 보호 | 🔒 **뮤텍스** | 임계구간 + 소유권 필요 |
| 스레드 대기열 관리 | 🔗 **세마포어** | 우선순위 기반 순서 제어 |
| 파일 접근 제어 | 🔒 **뮤텍스** | 한 번에 한 명만 + Priority Donation |
| 작업 완료 알림 | 🔗 **세마포어** | 이벤트 동기화 |

---

## 🏗️ 구현한 핵심 데이터 구조

### **Thread 구조체 확장**

```c
struct thread {
    /* 기존 필드들 */
    int priority;                    // 스케줄링에 사용되는 우선순위
    
    /* Priority Donation을 위해 새로 추가한 필드들 */
    int base_priority;               // 원본 우선순위 (사용자 설정값)
    int eff_priority;                // 유효 우선순위 (기부받은 우선순위 포함)
    struct list donators;            // 나에게 기부한 스레드들
    struct list_elem donate_elem;    // 내가 기부자가 될 때 사용
    struct lock *waiting_lock;       // 대기 중인 락 (체인 추적용)
    struct list held_locks;          // 보유 중인 락들
};
```

**🔑 핵심 아이디어:**
- `base_priority`: 사용자가 설정한 원래 값
- `eff_priority`: 기부를 반영한 실제 유효값
- `donators`: 우선순위 순으로 정렬된 기부자 리스트

---

## 🔧 구현한 핵심 함수들

### **1. 유효 우선순위 재계산**

```c
void recompute_eff_priority(struct thread *t) {
    ASSERT(t != NULL);
    
    int max_priority = t->base_priority;         // 기본 우선순위부터 시작
    
    /* 모든 기부자들 중 최고 우선순위 찾기 */
    if (!list_empty(&t->donators)) {
        struct list_elem *e;
        for (e = list_begin(&t->donators); e != list_end(&t->donators); e = list_next(e)) {
            struct thread *donator = list_entry(e, struct thread, donate_elem);
            if (donator->eff_priority > max_priority) {  // 기부자의 유효 우선순위와 비교
                max_priority = donator->eff_priority;    // 최대값 선택
            }
        }
    }
    
    t->eff_priority = max_priority;              // 계산된 최고값으로 설정
}
```

**동작 원리:** `eff_priority = max(base_priority, max(모든 기부자들의 eff_priority))`

### **2. 체인 우선순위 전파**

```c
void propagate_eff_priority() {
    enum intr_level old = intr_disable();        // 원자성 보장
    struct thread *cur = thread_current();      // 기부자
    struct lock *w_lock = cur->waiting_lock;     // 대기 중인 락
    int depth = 0;
    const int MAX_DEPTH = 8;                     // 무한 루프 방지
    
    /* 락 체인을 따라 우선순위 전파 */
    while ((w_lock = cur->waiting_lock) && w_lock->holder && depth < MAX_DEPTH) {
        depth++;
        struct thread *w_lock_holder = w_lock->holder;  // 수혜자

        /* 1. 기부자 리스트에 추가/재정렬 */
        bool is_donator = is_contain(&w_lock_holder->donators, &cur->donate_elem);
        if (!list_empty(&w_lock_holder->donators) && is_donator) {
            list_remove(&cur->donate_elem);      // 기존 위치에서 제거
        }
        list_insert_ordered(&w_lock_holder->donators, &cur->donate_elem, 
                           higher_priority_donate, NULL);  // 우선순위 순으로 재삽입

        /* 2. 전파할 가치가 있는지 확인 */
        int before_eff = w_lock_holder->eff_priority;
        if (cur->eff_priority <= before_eff) break;      // 더 낮으면 중단

        /* 3. 유효 우선순위 재계산 */
        recompute_eff_priority(w_lock_holder);

        /* 4. 실제로 변했는지 확인 */
        if (w_lock_holder->eff_priority == before_eff) break;
        if (w_lock_holder->status == THREAD_READY) {
            requeue_ready_list(w_lock_holder);   // Ready queue 재정렬
        }
        
        /* 5. 체인 따라 계속 전파 */
        cur = w_lock_holder;                     // 다음 단계로 이동
    }
    
    intr_set_level(old);
}
```

**동작 원리:** A→B→C 락 체인에서 A의 우선순위가 C까지 전파

### **3. 뮤텍스 획득 (Priority Donation)**

```c
void lock_acquire(struct lock *lock) {
    enum intr_level old = intr_disable();
    struct thread *cur = thread_current();
    
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(!lock_held_by_current_thread(lock));

    /* Priority Donation 로직 */
    if (lock->holder != NULL) {                  // 락이 이미 보유됨
        cur->waiting_lock = lock;                // 대기 락 설정
        
        /* 락 보유자의 기부자 리스트에 추가 */
        list_insert_ordered(&lock->holder->donators, &cur->donate_elem,
                           higher_priority_donate, NULL);
        
        /* 체인을 통한 우선순위 기부 */
        propagate_eff_priority();                // 우선순위 전파 시작
    }

    sema_down(&lock->semaphore);                 // 세마포어로 실제 대기 (뮤텍스 핵심!)
    
    /* 락 획득 완료 */
    cur->waiting_lock = NULL;
    lock->holder = cur;                          // 새로운 소유자
    list_push_back(&cur->held_locks, &lock->elem);  // 보유 락 리스트에 추가
    
    intr_set_level(old);
}
```

**핵심 흐름:**
1. **Priority Donation**: 락 보유자에게 우선순위 기부
2. **세마포어 대기**: 실제 블로킹은 세마포어가 담당
3. **소유권 설정**: 깨어나면 새로운 락 보유자가 됨

### **4. 뮤텍스 해제 (Priority Recovery)**

```c
void lock_release(struct lock *lock) {
    ASSERT(lock != NULL);
    ASSERT(lock_held_by_current_thread(lock));   // 소유권 검증 (뮤텍스 핵심!)
    
    enum intr_level old = intr_disable();
    struct thread *cur = lock->holder;
    int before_eff_priority = cur->eff_priority;

    /* 1. 이 락과 관련된 기부자들만 제거 */
    if (!list_empty(&cur->donators)) {
        remove_donators_related_lock(cur, lock); // 선별적 제거
        recompute_eff_priority(cur);             // 남은 기부자들로 재계산
    }
    
    /* 2. 스케줄링 갱신 */
    if ((before_eff_priority != cur->eff_priority) && cur->status == THREAD_READY) {
        requeue_ready_list(cur);                 // Ready queue 재정렬
    }
    
    /* 3. 락 해제 */
    list_remove(&lock->elem);                    // 보유 락 리스트에서 제거
    lock->holder = NULL;                         // 소유권 해제
    sema_up(&lock->semaphore);                   // 대기자 깨우기 (세마포어 활용!)
    
    intr_set_level(old);
}
```

**핵심 흐름:**
1. **선별적 회수**: 해당 락 관련 기부자만 제거
2. **우선순위 재계산**: 남은 기부자들로 새로운 유효 우선순위 계산
3. **세마포어 해제**: 대기 중인 스레드 중 하나 깨우기

### **5. 세마포어 우선순위 스케줄링**

```c
void sema_up(struct semaphore *sema) {
    enum intr_level old_level;
    struct thread *next = NULL;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    
    /* 우선순위 기반 스레드 선택 */
    if (!list_empty(&sema->waiters)) {
        list_sort(&sema->waiters, thread_priority_compare, NULL);  // 핵심 개선!
        next = list_entry(list_pop_front(&sema->waiters), struct thread, elem);
        thread_unblock(next);                    // 최고 우선순위 깨우기
    }
    sema->value++;
    intr_set_level(old_level);
    
    /* 더 높은 우선순위를 깨웠으면 즉시 양보 */
    if (next && next->eff_priority > thread_current()->eff_priority) {
        if (intr_context()) 
            intr_yield_on_return();
        else 
            thread_yield();                      // 즉시 선점
    }
}
```

**핵심 개선:** FIFO → Priority-based 스케줄링 + 즉시 선점

### **6. 기부 상태에서 우선순위 설정**

```c
void thread_set_priority(int new_priority) {
    struct thread *cur = thread_current();
    
    enum intr_level old = intr_disable();
    cur->base_priority = new_priority;           // 기본 우선순위만 변경 (핵심!)
    
    /* 기부를 고려한 유효 우선순위 재계산 */
    int before = cur->eff_priority;
    recompute_eff_priority(cur);                 // 기부와 새 기본값 중 최대값
    if (cur->eff_priority == before) {
        intr_set_level(old);
        return;
    }
    
    /* 더 높은 우선순위가 Ready에 있으면 양보 */
    if (!list_empty(&ready_list)) {
        struct thread *first = list_entry(list_front(&ready_list), struct thread, elem);
        if (first->eff_priority > cur->eff_priority) {
            intr_set_level(old);
            thread_yield();
            return;
        }
    }
    intr_set_level(old);
}
```

**핵심:** 기부받은 상태에서도 **기부받은 우선순위는 유지**, 기본 우선순위만 변경

---

## 🧪 테스트별 완벽 분석

### **priority-donate-one (기본 기부)**
```
Main(31) + Lock1 → ThreadA(32) 대기 → ThreadB(33) 대기
결과: Main이 33으로 승격 → Lock 해제 시 B(33) → A(32) 순서 실행
```
**학습 포인트:** 뮤텍스의 Priority Donation + 세마포어의 우선순위 스케줄링

### **priority-donate-multiple (다중 락)**
```
Main(31) + LockA + LockB → ThreadA(32) LockA 대기 → ThreadB(33) LockB 대기
결과: Main이 33으로 승격 → LockB 해제 시 32로 감소 → LockA 해제 시 31로 복구
```
**학습 포인트:** 락별 독립적인 기부자 관리 + 선별적 회수

### **priority-sema (순수 세마포어)**
```
10개 스레드가 다양한 우선순위로 semaphore 대기
Main이 sema_up() 호출 → 가장 높은 우선순위부터 순서대로 깨어남
```
**학습 포인트:** 세마포어의 우선순위 기반 스케줄링 (Priority Donation 없이)

### **priority-donate-sema (복합 시나리오)**
```
L(32): Lock 획득 → Semaphore 대기
M(34): Semaphore 대기  
H(36): Lock 요청 → L에게 Priority Donation → L이 36으로 실행
Main: sema_up() → L 깨어남 → Lock 해제 → H 실행 → sema_up() → M 실행
```
**학습 포인트:** 뮤텍스(상호배제) + 세마포어(이벤트 동기화) 복합 사용

### **priority-donate-chain (체인 전파)**
```
Thread7(21) → Lock6 → Thread6(18) → Lock5 → ... → Lock0 → Main(3)
결과: Thread7의 우선순위 21이 체인을 따라 Main까지 전파
```
**학습 포인트:** 락 dependency를 따른 체인 Priority Donation

---

## 🎯 핵심 설계 원칙

### **1. 완벽한 구분 기준**
```
🔒 뮤텍스 = 임계구간 한 스레드씩 독점 + Priority Donation
🔗 세마포어 = 우선순위로 양보하고 순서 결정 + 이벤트 동기화
```

### **2. 불변 조건 (Invariant)**
```
eff_priority = max(base_priority, max(donators의 모든 eff_priority))
```

### **3. 핵심 최적화**
- **조기 종료**: 이미 높은 우선순위면 전파 중단
- **선별적 회수**: 락별로 관련 기부자만 정확히 제거
- **즉시 선점**: 우선순위 변화 시 바로 스케줄링
- **깊이 제한**: 체인 전파 최대 8단계로 무한 루프 방지

---

## 🏆 최종 성과

### **✅ 완료된 모든 구현**
- **Priority Donation**: 기본 → 다중 → 체인 → 복합 시나리오
- **Priority Scheduling**: 뮤텍스, 세마포어, Condition Variable 모두 지원
- **Priority Inheritance**: 기부 중 기본 우선순위 변경 올바른 처리
- **원자성 보장**: 모든 중요 연산에서 인터럽트 비활성화
- **일관성 유지**: 모든 상황에서 불변 조건 만족

### **🔥 핵심 깨달음**
1. **뮤텍스**: 소유권 + 임계구간 + Priority Donation
2. **세마포어**: 동기화 + 우선순위 순서 + 이벤트 신호
3. **통합 시스템**: 상호 보완적 역할로 완전한 동기화 구현

**이제 Pintos Priority Scheduling의 완전한 마스터입니다!** 🚀

---

*"임계구간은 뮤텍스로, 우선순위 순서는 세마포어로 - 완벽한 동기화의 핵심!"* ✨
