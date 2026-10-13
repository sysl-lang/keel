/* A static aarch64 Linux program, like threads.c: no C library, no keel header, only raw system calls by
   Linux's numbers — but built WITHOUT -mgeneral-regs-only, because what it checks is that a user program
   may use the FP/SIMD unit and that every thread keeps its own q0-q31, FPSR and FPCR:

   - a new process starts with all of them zero (`_start` stores them before any C runs);
   - eight threads made by clone (musl's pthread_create flags, as threads.c) on four cores each load
     distinct values into all 32 q registers and a distinct FPCR and FPSR, run a long chain of SIMD adds
     through every register — long enough to be preempted and resumed, on whatever core takes it next —
     and store them back; each sum is checked against the same chain computed in C;
   - fork in the middle of such a chain: the child goes on from a copy of the parent's registers, and
     parent and child both finish their chains (the child a longer one) with exact sums;
   - a SIGUSR1 sent to itself in the middle of a chain runs a handler that overwrites every q register,
     FPSR and FPCR; after rt_sigreturn the chain finishes with the interrupted values;
   - execve of itself (`linuxfp fresh`) while every q register holds a value: the new image starts with
     them zero.

   The chains are assembly so that the values are in the registers, and nowhere else, for their whole
   length. scripts/boot-test.sh builds it as threads.c minus -mgeneral-regs-only and puts it on the disk
   as /bin/linuxfp. */

typedef unsigned long u64;
typedef unsigned int u32;

#define SYS_write 64
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_futex 98
#define SYS_kill 129
#define SYS_rt_sigaction 134
#define SYS_getpid 172
#define SYS_execve 221
#define SYS_wait4 260
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

#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | \
                      CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

#define FUTEX_WAIT 0
#define FUTEX_PRIVATE 128

#define SIGUSR1 10
#define SA_RESTORER 0x04000000

#define THREADS 8
#define ROUNDS 1000000
#define STACK_WORDS 2048 /* 16 KiB */

/* What the registers are loaded from and stored to: Linux's user_fpsimd_state. */
struct fpregs {
    u64 q[64]; /* q0..q31, low word first */
    u32 fpsr;
    u32 fpcr;
    u32 reserved[2];
} __attribute__((aligned(16)));

/* --- the assembly: chains of SIMD adds with every register live --------------------------------- */

#define LOAD_ALL(r)                                                                                     \
    "ldp q0, q1, [" r ", #0]\n ldp q2, q3, [" r ", #32]\n ldp q4, q5, [" r ", #64]\n"                     \
    "ldp q6, q7, [" r ", #96]\n ldp q8, q9, [" r ", #128]\n ldp q10, q11, [" r ", #160]\n"              \
    "ldp q12, q13, [" r ", #192]\n ldp q14, q15, [" r ", #224]\n ldp q16, q17, [" r ", #256]\n"         \
    "ldp q18, q19, [" r ", #288]\n ldp q20, q21, [" r ", #320]\n ldp q22, q23, [" r ", #352]\n"         \
    "ldp q24, q25, [" r ", #384]\n ldp q26, q27, [" r ", #416]\n ldp q28, q29, [" r ", #448]\n"         \
    "ldp q30, q31, [" r ", #480]\n"                                                                     \
    "ldr w11, [" r ", #512]\n msr fpsr, x11\n ldr w11, [" r ", #516]\n msr fpcr, x11\n"

#define STORE_ALL(r)                                                                                    \
    "stp q0, q1, [" r ", #0]\n stp q2, q3, [" r ", #32]\n stp q4, q5, [" r ", #64]\n"                     \
    "stp q6, q7, [" r ", #96]\n stp q8, q9, [" r ", #128]\n stp q10, q11, [" r ", #160]\n"              \
    "stp q12, q13, [" r ", #192]\n stp q14, q15, [" r ", #224]\n stp q16, q17, [" r ", #256]\n"         \
    "stp q18, q19, [" r ", #288]\n stp q20, q21, [" r ", #320]\n stp q22, q23, [" r ", #352]\n"         \
    "stp q24, q25, [" r ", #384]\n stp q26, q27, [" r ", #416]\n stp q28, q29, [" r ", #448]\n"         \
    "stp q30, q31, [" r ", #480]\n"                                                                     \
    "mrs x11, fpsr\n str w11, [" r ", #512]\n mrs x11, fpcr\n str w11, [" r ", #516]\n"

/* One round: q[i] += q[i-1] for i = 1..31, then q[0] += q[31] — one chain through all 32 registers. */
#define ADD(a, b) "add v" #a ".2d, v" #a ".2d, v" #b ".2d\n"
#define ROUND                                                                                           \
    ADD(1, 0) ADD(2, 1) ADD(3, 2) ADD(4, 3) ADD(5, 4) ADD(6, 5) ADD(7, 6) ADD(8, 7) ADD(9, 8) ADD(10, 9)  \
    ADD(11, 10) ADD(12, 11) ADD(13, 12) ADD(14, 13) ADD(15, 14) ADD(16, 15) ADD(17, 16) ADD(18, 17)       \
    ADD(19, 18) ADD(20, 19) ADD(21, 20) ADD(22, 21) ADD(23, 22) ADD(24, 23) ADD(25, 24) ADD(26, 25)       \
    ADD(27, 26) ADD(28, 27) ADD(29, 28) ADD(30, 29) ADD(31, 30) ADD(0, 31)

/* d8-d15 are callee-saved. */
#define SAVE_D "stp d8, d9, [sp, #-64]!\n stp d10, d11, [sp, #16]\n stp d12, d13, [sp, #32]\n stp d14, d15, [sp, #48]\n"
#define RESTORE_D "ldp d10, d11, [sp, #16]\n ldp d12, d13, [sp, #32]\n ldp d14, d15, [sp, #48]\n ldp d8, d9, [sp], #64\n"

#define DUP(n) "dup v" #n ".2d, x9\n"

/* grind(r, n): load *r, n rounds, store *r. */
void grind(struct fpregs *r, long n);
/* fork_grind(r, before, after, more): load *r, `before` rounds, fork; the parent then runs `after`
   rounds, the child `after + more`; store *r. Answers fork's answer. */
long fork_grind(struct fpregs *r, long before, long after, long more);
/* signal_grind(r, before, after, pid): load *r, `before` rounds, kill(pid, SIGUSR1), `after` rounds,
   store *r. */
void signal_grind(struct fpregs *r, long before, long after, long pid);
/* exec_live(r, path, argv): load *r and execve(path, argv, 0) with them live. Answers only on failure. */
long exec_live(struct fpregs *r, const char *path, const char *const *argv);
/* The SIGUSR1 handler: every q register, FPSR and FPCR overwritten; `handled` counted. */
void fp_handler(int sig);
void restorer(void);
long raw_clone(u64 flags, void *stack, volatile u32 *ptid, void *tls, volatile u32 *ctid, void (*fn)(void *),
    void *arg);

/* The registers as `_start` found them. Not static: only the assembly writes it, and the compiler must
   not take it for a zero it can fold. */
struct fpregs entry_fp;
volatile u32 handled;

__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "    adrp x9, entry_fp\n"
        "    add x9, x9, :lo12:entry_fp\n" STORE_ALL("x9")
        "    mov x0, sp\n"
        "    b main_with\n"

        ".global grind\n"
        "grind:\n" SAVE_D LOAD_ALL("x0")
        "    cbz x1, 2f\n"
        "1:\n" ROUND
        "    subs x1, x1, #1\n"
        "    b.ne 1b\n"
        "2:\n" STORE_ALL("x0") RESTORE_D
        "    ret\n"

        ".global fork_grind\n"
        "fork_grind:\n" SAVE_D
        "    mov x9, x0\n"
        "    mov x12, x2\n"
        "    mov x13, x3\n" LOAD_ALL("x9")
        "    cbz x1, 2f\n"
        "1:\n" ROUND
        "    subs x1, x1, #1\n"
        "    b.ne 1b\n"
        "2:  mov x0, #17\n" /* SIGCHLD: a fork */
        "    mov x1, #0\n"
        "    mov x2, #0\n"
        "    mov x3, #0\n"
        "    mov x4, #0\n"
        "    mov x8, #220\n"
        "    svc #0\n"
        "    mov x10, x0\n"
        "    cbnz x0, 3f\n"
        "    add x12, x12, x13\n"
        "3:  cbz x12, 5f\n"
        "4:\n" ROUND
        "    subs x12, x12, #1\n"
        "    b.ne 4b\n"
        "5:\n" STORE_ALL("x9") RESTORE_D
        "    mov x0, x10\n"
        "    ret\n"

        ".global signal_grind\n"
        "signal_grind:\n" SAVE_D
        "    mov x9, x0\n"
        "    mov x12, x2\n"
        "    mov x13, x3\n" LOAD_ALL("x9")
        "    cbz x1, 2f\n"
        "1:\n" ROUND
        "    subs x1, x1, #1\n"
        "    b.ne 1b\n"
        "2:  mov x0, x13\n"
        "    mov x1, #10\n" /* SIGUSR1 */
        "    mov x8, #129\n"
        "    svc #0\n"
        "    cbz x12, 4f\n"
        "3:\n" ROUND
        "    subs x12, x12, #1\n"
        "    b.ne 3b\n"
        "4:\n" STORE_ALL("x9") RESTORE_D
        "    ret\n"

        ".global exec_live\n"
        "exec_live:\n" SAVE_D
        "    mov x9, x0\n"
        "    mov x13, x1\n"
        "    mov x14, x2\n" LOAD_ALL("x9")
        "    mov x0, x13\n"
        "    mov x1, x14\n"
        "    mov x2, #0\n"
        "    mov x8, #221\n"
        "    svc #0\n" RESTORE_D
        "    ret\n"

        ".global fp_handler\n"
        "fp_handler:\n"
        "    mov x9, #0x5aa5\n"
        "    movk x9, #0xc33c, lsl #16\n"
        "    movk x9, #0x0ff0, lsl #32\n"
        "    movk x9, #0x9669, lsl #48\n" DUP(0) DUP(1) DUP(2) DUP(3) DUP(4) DUP(5) DUP(6) DUP(7) DUP(8) DUP(9)
            DUP(10) DUP(11) DUP(12) DUP(13) DUP(14) DUP(15) DUP(16) DUP(17) DUP(18) DUP(19) DUP(20) DUP(21)
                DUP(22) DUP(23) DUP(24) DUP(25) DUP(26) DUP(27) DUP(28) DUP(29) DUP(30) DUP(31)
        "    mov x9, #0x3c00000\n" /* RMode toward zero, FZ, DN */
        "    msr fpcr, x9\n"
        "    mov x9, #0x9f\n" /* every cumulative flag */
        "    msr fpsr, x9\n"
        "    adrp x9, handled\n"
        "    ldr w10, [x9, :lo12:handled]\n"
        "    add w10, w10, #1\n"
        "    str w10, [x9, :lo12:handled]\n"
        "    ret\n"

        ".global restorer\n"
        "restorer:\n"
        "    mov x8, #139\n"
        "    svc #0\n"

        /* clone with fn and arg on the new stack, as threads.c's raw_clone (musl's __clone). */
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
        "    brk #0\n");

/* The chain grind runs, in C: what the registers must hold after `n` rounds. */
static void reference(struct fpregs *r, long n) {
    for (long k = 0; k < n; k++) {
        for (int i = 1; i < 32; i++) {
            r->q[2 * i] += r->q[2 * i - 2];
            r->q[2 * i + 1] += r->q[2 * i - 1];
        }
        r->q[0] += r->q[62];
        r->q[1] += r->q[63];
    }
}

/* Distinct values in every register for `seed`, and an FPCR and FPSR of its own. */
static void seeded(struct fpregs *r, u64 seed) {
    for (int i = 0; i < 32; i++) {
        r->q[2 * i] = seed * 0x9e3779b97f4a7c15ul + (u64)i;
        r->q[2 * i + 1] = (seed + 1) * 0xc2b2ae3d27d4eb4ful ^ ((u64)i << 40);
    }
    r->fpcr = (u32)(((seed & 3) << 22) | (((seed >> 2) & 1) << 24) | (1u << 25));
    r->fpsr = (u32)((seed % 31) + 1);
    r->reserved[0] = r->reserved[1] = 0;
}

/* The first word that differs, or -1. */
static long differs(const struct fpregs *a, const struct fpregs *b) {
    for (int i = 0; i < 64; i++)
        if (a->q[i] != b->q[i])
            return i;
    if (a->fpsr != b->fpsr)
        return 64;
    if (a->fpcr != b->fpcr)
        return 65;
    return -1;
}

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
    put("linuxfp: FAILED ");
    put(what);
    put(" ");
    put_number(value);
    put("\n");
    flush();
    sc(SYS_exit_group, 1, 0, 0, 0, 0);
}

/* --- threads ------------------------------------------------------------------------------------ */

static struct fpregs results[THREADS];
static volatile long wrong_word[THREADS];
static u64 stacks[THREADS][STACK_WORDS] __attribute__((aligned(16)));
static u64 tls[THREADS];
static volatile u32 child_tid[THREADS];
static volatile u32 parent_tid[THREADS];

static void worker(void *arg) {
    long i = (long)arg;
    struct fpregs expect;
    seeded(&results[i], (u64)i + 1);
    seeded(&expect, (u64)i + 1);
    grind(&results[i], ROUNDS);
    reference(&expect, ROUNDS);
    wrong_word[i] = differs(&results[i], &expect);
}

static void start(long i) {
    child_tid[i] = 0xffffffff;
    long tid = raw_clone(THREAD_FLAGS, &stacks[i][STACK_WORDS], &parent_tid[i], &tls[i], &child_tid[i], worker,
        (void *)i);
    if (tid <= 0)
        fail("clone answered", tid);
}

static void join(long i) {
    u32 v;
    while ((v = child_tid[i]) != 0)
        sc(SYS_futex, (long)&child_tid[i], FUTEX_WAIT | FUTEX_PRIVATE, v, 0, 0);
}

/* keel's `procs` record (keel.abi's ProcInfo), as threads.c reads it. */
struct proc_info {
    u64 pid, ppid, pgid;
    u32 uid, ended;
    char name[16];
    u32 cores, parallel;
};
static struct proc_info procs[64];

static void threads(long pid) {
    for (long i = 0; i < THREADS; i++)
        start(i);
    /* The first thread computes in the unit too while they run. */
    struct fpregs mine, expect;
    seeded(&mine, 99);
    seeded(&expect, 99);
    grind(&mine, ROUNDS);
    reference(&expect, ROUNDS);
    long w = differs(&mine, &expect);
    if (w >= 0)
        fail("the first thread's sum differs at word", w);
    for (long i = 0; i < THREADS; i++)
        join(i);
    for (long i = 0; i < THREADS; i++)
        if (wrong_word[i] >= 0)
            fail("a thread's sum differs; thread x 100 + word:", i * 100 + wrong_word[i]);
    long n = sc(SYS_procs, (long)procs, 64, 0, 0, 0);
    long cores = 0;
    for (long i = 0; i < n; i++)
        if ((long)procs[i].pid == pid)
            for (u32 c = procs[i].cores; c != 0; c &= c - 1)
                cores++;
    if (cores < 2)
        fail("the threads ran on this many cores:", cores);
    put("linuxfp: 8 threads x 1000000 rounds through q0-q31 on 4 cores: every sum exact, FPCR and FPSR their own\n");
    flush();
}

/* --- fork --------------------------------------------------------------------------------------- */

#define HALF (ROUNDS / 2)
#define MORE 1000

static void forking(void) {
    struct fpregs r, expect;
    seeded(&r, 41);
    seeded(&expect, 41);
    long child = fork_grind(&r, HALF, HALF, MORE);
    if (child < 0)
        fail("fork answered", child);
    reference(&expect, child == 0 ? 2 * HALF + MORE : 2 * HALF);
    long w = differs(&r, &expect);
    if (child == 0)
        sc(SYS_exit_group, w < 0 ? 0 : 10 + w, 0, 0, 0, 0);
    if (w >= 0)
        fail("the parent's sum after fork differs at word", w);
    int status = -1;
    long r4 = sc(SYS_wait4, child, (long)&status, 0, 0, 0);
    if (r4 != child)
        fail("wait4 answered", r4);
    if (status != 0)
        fail("the child's sum after fork differs; its status", status);
    put("linuxfp: fork: parent and child each carried the q registers on; both sums exact\n");
    flush();
}

/* --- a handler that uses the unit --------------------------------------------------------------- */

struct ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static void signalled(long pid) {
    struct ksigaction act = {(u64)fp_handler, SA_RESTORER, (u64)restorer, 0};
    long r = sc(SYS_rt_sigaction, SIGUSR1, (long)&act, 0, 8, 0);
    if (r != 0)
        fail("rt_sigaction answered", r);
    struct fpregs regs, expect;
    seeded(&regs, 77);
    seeded(&expect, 77);
    signal_grind(&regs, HALF, HALF, pid);
    reference(&expect, 2 * HALF);
    if (handled != 1)
        fail("the handler ran this many times:", handled);
    long w = differs(&regs, &expect);
    if (w >= 0)
        fail("the sum across the handler differs at word", w);
    put("linuxfp: a handler that overwrote q0-q31, FPSR and FPCR left the interrupted sums exact\n");
    flush();
}

/* --- the start, and execve ---------------------------------------------------------------------- */

static int all_zero(const struct fpregs *r) {
    for (int i = 0; i < 64; i++)
        if (r->q[i] != 0)
            return 0;
    return r->fpsr == 0 && r->fpcr == 0;
}

void main_with(long *sp) {
    int fresh = sp[0] > 1 && ((const char *)sp[2])[0] == 'f';
    if (!all_zero(&entry_fp))
        fail(fresh ? "execve left a register set; FPCR" : "a new process started with a register set; FPCR",
            entry_fp.fpcr);
    if (fresh) {
        put("linuxfp: execve zeroed every q register, FPSR and FPCR\n");
        flush();
        sc(SYS_exit_group, 0, 0, 0, 0, 0);
    }
    put("linuxfp: a new process starts with every q register, FPSR and FPCR zero\n");
    flush();

    long pid = sc(SYS_getpid, 0, 0, 0, 0, 0);
    threads(pid);
    forking();
    signalled(pid);

    struct fpregs live;
    seeded(&live, 5);
    static const char path[] = "/bin/linuxfp";
    const char *const argv[] = {"linuxfp", "fresh", 0};
    long r = exec_live(&live, path, argv);
    fail("execve answered", r);
}
