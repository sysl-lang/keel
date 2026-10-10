/* A static aarch64 Linux program, like hello.c: no C library, no keel header, only raw system calls by
   Linux's numbers. It is what a C library's pthreads would do, by hand:

   - four threads made by clone with musl's pthread_create flags, each on a stack of its own with a TLS
     pointer of its own (CLONE_SETTLS), add to one counter under a futex lock (Drepper's three-state
     mutex) 100,000 times each, checking TPIDR_EL0 is still their own every round;
   - the first thread joins each the way pthread_join does: a futex wait on the word CLONE_CHILD_CLEARTID
     names, which the kernel zeroes and wakes when the thread ends;
   - futex's other answers: EAGAIN for a word that changed, ETIMEDOUT after a relative timeout, ENOSYS for
     an operation keel does not have;
   - a signal sent to the process goes to the one thread not blocking it, waking its futex wait (EINTR);
   - keel's own `procs` (1025) says how many of its threads were ever running at once: at least 2, or the
     threads never ran in parallel;
   - with an argument (`linuxthreads exec`), a thread that is not the first execs /bin/linuxhello
     while another spins; execve ends them first, and the pid stays (exec_from_a_thread);
   - then the first thread ends with exit (93) while two others run on, one spinning in user mode, the
     other ending the whole group with exit_group(6): the status the shell reports is 6 only if the
     spinning thread was ended too and the process ended with its last thread.

   scripts/boot-test.sh builds it as it builds hello.c (plus -mno-outline-atomics: the atomics are inline
   instructions, not calls into a runtime) and puts it on the disk as /bin/linuxthreads. */

typedef unsigned long u64;
typedef unsigned int u32;

#define SYS_write 64
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_futex 98
#define SYS_kill 129
#define SYS_rt_sigaction 134
#define SYS_rt_sigprocmask 135
#define SYS_execve 221
#define SYS_getpid 172
#define SYS_gettid 178
#define SYS_procs 1025

#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000
#define CLONE_SYSVSEM 0x40000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_DETACHED 0x400000
/* musl's pthread_create, exactly. */
#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | \
    CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 128

#define SIGUSR1 10
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SA_RESTORER 0x04000000

#define EINTR 4
#define EAGAIN 11
#define ENOSYS 38
#define ETIMEDOUT 110

#define WORKERS 4
#define ROUNDS 100000
#define STACK_WORDS 2048 /* 16 KiB */
#define THREADS (WORKERS + 3)

static long sc(long nr, long a0, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

/* clone(flags, stack, ptid, tls, ctid) with the new thread calling fn(arg) on its own stack and then
   exit(0), as musl's __clone does: fn and arg go on the new stack before the call, and the new thread
   (x0 = 0) pops them from there, never touching the caller's frame. */
long raw_clone(u64 flags, void *stack, volatile u32 *ptid, void *tls, volatile u32 *ctid, void (*fn)(void *),
    void *arg);
__asm__(".text\n"
        ".global raw_clone\n"
        "raw_clone:\n"
        "    stp x5, x6, [x1, #-16]!\n"
        "    mov x8, #220\n"
        "    svc #0\n"
        "    cbz x0, 1f\n"
        "    ret\n"
        "1:  ldp x1, x0, [sp], #16\n"
        "    blr x1\n"
        "    mov x0, #0\n"
        "    mov x8, #93\n"
        "    svc #0\n"
        "    brk #0\n"
        ".global restorer\n"
        "restorer:\n"
        "    mov x8, #139\n"
        "    svc #0\n");
void restorer(void);

static long futex(volatile u32 *word, long op, u32 val, const long *timeout) {
    return sc(SYS_futex, (long)word, op | FUTEX_PRIVATE, val, (long)timeout, 0);
}

static u64 thread_pointer(void) {
    u64 v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

/* --- output ------------------------------------------------------------------------------------- */

static char out[256];
static u64 out_len;

static void put(const char *s) {
    while (*s != 0 && out_len < sizeof(out))
        out[out_len++] = *s++;
}

static void put_number(long n) {
    char digits[20];
    int k = 0;
    if (n < 0) {
        put("-");
        n = -n;
    }
    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    while (k > 0 && out_len < sizeof(out))
        out[out_len++] = digits[--k];
}

static void flush(void) {
    sc(SYS_write, 1, (long)out, (long)out_len, 0, 0);
    out_len = 0;
}

static void fail(const char *what, long value) {
    put("linuxthreads: FAILED ");
    put(what);
    put(" ");
    put_number(value);
    put("\n");
    flush();
    sc(SYS_exit_group, 1, 0, 0, 0, 0);
}

/* --- the futex lock: 0 free, 1 held, 2 held with waiters ----------------------------------------- */

static volatile u32 lock_word;
static u64 counter;

static void lock(volatile u32 *m) {
    u32 c = 0;
    if (__atomic_compare_exchange_n(m, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex(m, FUTEX_WAIT, 2, 0);
        c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE);
    }
}

static void unlock(volatile u32 *m) {
    if (__atomic_fetch_sub(m, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(m, 0, __ATOMIC_RELEASE);
        futex(m, FUTEX_WAKE, 1, 0);
    }
}

/* --- the threads -------------------------------------------------------------------------------- */

struct tls {
    struct tls *self;
    long index;
};

static struct tls tls[THREADS];
static u64 stacks[THREADS][STACK_WORDS] __attribute__((aligned(16)));
static volatile u32 child_tid[THREADS]; /* CLONE_CHILD_CLEARTID: zeroed and woken at the thread's end */
static volatile u32 parent_tid[THREADS]; /* CLONE_PARENT_SETTID: the thread's id */
static volatile long tid_seen[THREADS];
static volatile u32 tls_wrong;

static void worker(void *arg) {
    struct tls *me = arg;
    tid_seen[me->index] = sc(SYS_gettid, 0, 0, 0, 0, 0);
    for (long i = 0; i < ROUNDS; i++) {
        lock(&lock_word);
        counter++;
        unlock(&lock_word);
        if (thread_pointer() != (u64)me)
            __atomic_fetch_add(&tls_wrong, 1, __ATOMIC_RELAXED);
    }
}

static long start(int i, void (*fn)(void *)) {
    tls[i].self = &tls[i];
    tls[i].index = i;
    child_tid[i] = 0xffffffff;
    long tid = raw_clone(THREAD_FLAGS, &stacks[i][STACK_WORDS], &parent_tid[i], &tls[i], &child_tid[i], fn, &tls[i]);
    if (tid <= 0)
        fail("clone answered", tid);
    if (parent_tid[i] != (u32)tid)
        fail("CLONE_PARENT_SETTID wrote", parent_tid[i]);
    return tid;
}

/* pthread_join's wait: until the kernel has zeroed the word, sleeping on it while it has not. */
static void join(int i) {
    u32 v;
    while ((v = child_tid[i]) != 0)
        futex(&child_tid[i], FUTEX_WAIT, v, 0);
}

/* --- a signal for the process, taken by the one thread that does not block it -------------------- */

struct ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static volatile long handled_by;
static volatile u32 sig_ready;
static volatile u32 sig_word;
static volatile long sig_eintr;

static void on_usr1(int sig) {
    (void)sig;
    handled_by = sc(SYS_gettid, 0, 0, 0, 0, 0);
}

static void signal_taker(void *arg) {
    (void)arg;
    u64 usr1 = 1ul << (SIGUSR1 - 1);
    sc(SYS_rt_sigprocmask, SIG_UNBLOCK, (long)&usr1, 0, 8, 0);
    long second[2] = {1, 0};
    while (handled_by == 0) {
        sig_ready = 1;
        long r = futex(&sig_word, FUTEX_WAIT, 0, second);
        if (r == -EINTR)
            sig_eintr = 1;
    }
}

/* --- the end: exit from the first thread, exit_group from another while a third spins ------------- */

static void spinner(void *arg) {
    (void)arg;
    for (;;)
        __asm__ volatile("" ::: "memory");
}

static void last(void *arg) {
    (void)arg;
    static volatile u32 never;
    long fifty_ms[2] = {0, 50000000};
    futex(&never, FUTEX_WAIT, 0, fifty_ms);
    put("linuxthreads: the first thread has exited; exit_group ends the rest\n");
    flush();
    sc(SYS_exit_group, 6, 0, 0, 0, 0);
}

/* keel's `procs` record (keel.abi's ProcInfo). */
struct proc_info {
    u64 pid, ppid, pgid;
    u32 uid, ended;
    char name[16];
    u32 cores, parallel;
};
static struct proc_info procs[64];

/* `linuxthreads exec`: a thread that is not the first replaces the program with /bin/linuxhello while
   another spins in user mode and the first waits in a futex. execve has to end both first (Linux's
   rule); linuxhello then prints its line with the same pid and exits 7. */
static const char hello_path[] = "/bin/linuxhello";

static void execer(void *arg) {
    (void)arg;
    static const char *argv[] = {hello_path, 0};
    long r = sc(SYS_execve, (long)hello_path, (long)argv, 0, 0, 0);
    fail("execve from a thread answered", r);
}

static void exec_from_a_thread(void) {
    start(0, spinner);
    start(1, execer);
    static volatile u32 never;
    futex(&never, FUTEX_WAIT, 0, 0);
    fail("the first thread outlived an execve", 0);
}

/* The program starts here, with SP at argc (Linux's layout): `_start` hands that to `main_with`. */
__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "    mov x0, sp\n"
        "    b main_with\n");

void main_with(long *sp) {
    if (sp[0] > 1)
        exec_from_a_thread();
    long pid = sc(SYS_getpid, 0, 0, 0, 0, 0);
    if (sc(SYS_gettid, 0, 0, 0, 0, 0) != pid)
        fail("the first thread's id is not the pid", 0);

    long tids[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        tids[i] = start(i, worker);
    for (int i = 0; i < WORKERS; i++)
        join(i);
    if (counter != (u64)WORKERS * ROUNDS)
        fail("total", (long)counter);
    if (tls_wrong != 0)
        fail("TPIDR_EL0 was another thread's, times:", tls_wrong);
    if (thread_pointer() != 0)
        fail("the first thread's TPIDR_EL0 changed to", (long)thread_pointer());
    for (int i = 0; i < WORKERS; i++) {
        if (tid_seen[i] != tids[i] || tids[i] == pid)
            fail("gettid in a thread answered", tid_seen[i]);
        for (int j = 0; j < i; j++)
            if (tids[j] == tids[i])
                fail("two threads have the id", tids[i]);
    }
    put("linuxthreads: 4 threads x 100000 under a futex lock: ");
    put_number((long)counter);
    put("; TLS and ids their own; joined\n");
    flush();

    static volatile u32 word = 7;
    long thirty_ms[2] = {0, 30000000};
    long r;
    if ((r = futex(&word, FUTEX_WAIT, 8, 0)) != -EAGAIN)
        fail("a wait on a changed word answered", r);
    if ((r = futex(&word, FUTEX_WAIT, 7, thirty_ms)) != -ETIMEDOUT)
        fail("a wait with a timeout answered", r);
    if ((r = futex(&word, FUTEX_WAKE, 1, 0)) != 0)
        fail("a wake with nobody waiting answered", r);
    if ((r = sc(SYS_futex, (long)&word, 9, 0, 0, 0)) != -ENOSYS)
        fail("futex op 9 answered", r);

    struct ksigaction act = {(u64)on_usr1, SA_RESTORER, (u64)restorer, 0};
    if ((r = sc(SYS_rt_sigaction, SIGUSR1, (long)&act, 0, 8, 0)) != 0)
        fail("rt_sigaction answered", r);
    u64 usr1 = 1ul << (SIGUSR1 - 1);
    sc(SYS_rt_sigprocmask, SIG_BLOCK, (long)&usr1, 0, 8, 0);
    long taker = start(WORKERS, signal_taker);
    while (sig_ready == 0)
        futex(&word, FUTEX_WAIT, 7, thirty_ms);
    futex(&word, FUTEX_WAIT, 7, thirty_ms);
    sc(SYS_kill, pid, SIGUSR1, 0, 0, 0);
    join(WORKERS);
    if (handled_by != taker)
        fail("SIGUSR1 was handled by thread", handled_by);
    if (!sig_eintr)
        fail("the taker's futex wait did not answer EINTR", 0);
    put("linuxthreads: futex EAGAIN, ETIMEDOUT, ENOSYS; the signal went to the thread not blocking it\n");
    flush();

    long n = sc(SYS_procs, (long)procs, 64, 0, 0, 0);
    long parallel = -1;
    for (long i = 0; i < n; i++)
        if ((long)procs[i].pid == pid)
            parallel = procs[i].parallel;
    if (parallel < 2)
        fail("the most threads running at once:", parallel);
    put("linuxthreads: threads ran on different cores at once\n");
    flush();

    start(WORKERS + 1, spinner);
    start(WORKERS + 2, last);
    sc(SYS_exit, 5, 0, 0, 0, 0);
    for (;;)
        ;
}
