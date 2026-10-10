/* A static aarch64 Linux program, like hello.c and threads.c: no C library, no keel header, only raw
   system calls by Linux's numbers. It is milestone 17's proof — what a C library's poll, select,
   clock_gettime, nanosleep and tcsetattr would do, by hand:

   - ppoll on a pipe sleeps until a thread of its own writes (POLLIN), and again until that thread closes
     the write end (POLLHUP); a ppoll with nothing to wake it times out in about its time, and one on a
     closed descriptor answers POLLNVAL; a write end with no reader is POLLERR;
   - pselect6 sleeps until a forked child writes, sees a write end as writable, and is EBADF for a
     descriptor that is not open;
   - ppoll's and pselect6's signal mask let a blocked SIGUSR1 in for the call alone: EINTR, the handler
     ran, and SIGUSR1 is blocked again after;
   - CLOCK_MONOTONIC advances across a nanosleep by at least the time slept, clock_nanosleep with
     TIMER_ABSTIME wakes after its time, CLOCK_REALTIME and gettimeofday read the RTC's date;
   - termios on the console: Linux's defaults, a TCGETS -> TCSETS -> TCGETS round trip, ENOTTY on a
     pipe; then raw mode, driven by the test's input: a read of three bytes typed with no newline and
     not echoed, Ctrl-C read as a byte with ISIG off, a VTIME read that times out; the settings put back.

   scripts/boot-test.sh builds it as it builds threads.c and puts it on the disk as /bin/linuxpoll. The
   bounds on times are wide on purpose: TCG is slow, and the point is that a wait ended for the right
   reason, not how fast. */

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_ioctl 29
#define SYS_close 57
#define SYS_pipe2 59
#define SYS_read 63
#define SYS_write 64
#define SYS_pselect6 72
#define SYS_ppoll 73
#define SYS_exit_group 94
#define SYS_futex 98
#define SYS_nanosleep 101
#define SYS_clock_gettime 113
#define SYS_clock_getres 114
#define SYS_clock_nanosleep 115
#define SYS_kill 129
#define SYS_rt_sigaction 134
#define SYS_rt_sigprocmask 135
#define SYS_gettimeofday 169
#define SYS_getpid 172
#define SYS_clone 220
#define SYS_wait4 260

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
#define SIGCHLD 17
#define FUTEX_WAIT 0
#define FUTEX_PRIVATE 128

#define POLLIN 0x1
#define POLLOUT 0x4
#define POLLERR 0x8
#define POLLHUP 0x10
#define POLLNVAL 0x20

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_COARSE 6
#define TIMER_ABSTIME 1

#define SIGUSR1 10
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SA_RESTORER 0x04000000

#define TCGETS 0x5401
#define TCSETS 0x5402
#define ICRNL 0x100
#define OPOST 0x1
#define ONLCR 0x4
#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8
#define VINTR 0
#define VERASE 2
#define VEOF 4
#define VTIME 5
#define VMIN 6

#define EINTR 4
#define EBADF 9
#define EINVAL 22
#define ENOTTY 25

#define MS 1000000L
#define SEC 1000000000L

static long sc(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

/* clone for a thread, as threads.c's (musl's __clone): fn(arg) on the new stack, then exit(0). */
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

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct timespec {
    long sec;
    long nsec;
};

struct timeval {
    long sec;
    long usec;
};

/* The kernel's struct termios (asm-generic): 36 bytes. */
struct ktermios {
    u32 iflag, oflag, cflag, lflag;
    u8 line;
    u8 cc[19];
};

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
    sc(SYS_write, 1, (long)out, (long)out_len, 0, 0, 0);
    out_len = 0;
}

static void say(const char *line) {
    put(line);
    flush();
}

static struct ktermios original;
static int raw_mode;

static void fail(const char *what, long value) {
    if (raw_mode)
        sc(SYS_ioctl, 0, TCSETS, (long)&original, 0, 0, 0);
    put("linuxpoll: FAILED ");
    put(what);
    put(" ");
    put_number(value);
    put("\n");
    flush();
    sc(SYS_exit_group, 1, 0, 0, 0, 0, 0);
}

static void check(int ok, const char *what, long value) {
    if (!ok)
        fail(what, value);
}

/* --- time --------------------------------------------------------------------------------------- */

static long clock_ns(int clock) {
    struct timespec t;
    long r = sc(SYS_clock_gettime, clock, (long)&t, 0, 0, 0, 0);
    check(r == 0, "clock_gettime answered", r);
    return t.sec * SEC + t.nsec;
}

static long now(void) {
    return clock_ns(CLOCK_MONOTONIC);
}

static void sleep_ns(long ns) {
    struct timespec t = {ns / SEC, ns % SEC};
    long r = sc(SYS_nanosleep, (long)&t, 0, 0, 0, 0, 0);
    check(r == 0, "nanosleep answered", r);
}

static long ppoll(struct pollfd *fds, long n, long timeout_ns, struct timespec *left) {
    struct timespec t = {timeout_ns / SEC, timeout_ns % SEC};
    long r = sc(SYS_ppoll, (long)fds, n, (long)&t, 0, 8, 0);
    if (left != 0)
        *left = t;
    return r;
}

/* --- a writer thread ---------------------------------------------------------------------------- */

static u64 stack[2048] __attribute__((aligned(16)));
static volatile u32 child_tid;
static volatile u32 parent_tid;
static int pipe_fds[2];

/* Sleep, write a byte, sleep, close the write end: one wake for data, one for the end of the writers. */
static void writer(void *arg) {
    (void)arg;
    sleep_ns(100 * MS);
    sc(SYS_write, pipe_fds[1], (long)"w", 1, 0, 0, 0);
    sleep_ns(100 * MS);
    sc(SYS_close, pipe_fds[1], 0, 0, 0, 0, 0);
}

static void join(void) {
    u32 v;
    while ((v = child_tid) != 0)
        sc(SYS_futex, (long)&child_tid, FUTEX_WAIT | FUTEX_PRIVATE, v, 0, 0, 0);
}

static void polling(void) {
    long r = sc(SYS_pipe2, (long)pipe_fds, 0, 0, 0, 0, 0);
    check(r == 0, "pipe2 answered", r);
    child_tid = 0xffffffff;
    long tid = raw_clone(THREAD_FLAGS, &stack[2048], &parent_tid, 0, &child_tid, writer, 0);
    check(tid > 0, "clone answered", tid);

    struct pollfd fds[2] = {{pipe_fds[0], POLLIN, 0}, {-1, POLLIN, 0x77}};
    struct timespec left;
    long t0 = now();
    r = ppoll(fds, 2, 5 * SEC, &left);
    long took = now() - t0;
    check(r == 1, "ppoll for the thread's write answered", r);
    check(fds[0].revents == POLLIN, "the pipe's revents were", fds[0].revents);
    check(fds[1].revents == 0, "a negative descriptor's revents were", fds[1].revents);
    check(took >= 50 * MS && took < 3 * SEC, "ppoll woke for the write after (ns)", took);
    check(left.sec >= 1 && left.sec <= 5, "ppoll left this much of its 5 s timeout (s)", left.sec);
    char b;
    r = sc(SYS_read, pipe_fds[0], (long)&b, 1, 0, 0, 0);
    check(r == 1 && b == 'w', "the read after POLLIN answered", r);

    t0 = now();
    r = ppoll(fds, 1, 5 * SEC, 0);
    took = now() - t0;
    check(r == 1 && fds[0].revents == POLLHUP, "ppoll for the thread's close: revents", fds[0].revents);
    check(took >= 50 * MS && took < 3 * SEC, "ppoll woke for the close after (ns)", took);
    join();
    sc(SYS_close, pipe_fds[0], 0, 0, 0, 0, 0);

    /* Nothing will come: the timeout ends it, in about its time. */
    int q[2];
    sc(SYS_pipe2, (long)q, 0, 0, 0, 0, 0);
    struct pollfd idle = {q[0], POLLIN, 0};
    t0 = now();
    r = ppoll(&idle, 1, 200 * MS, &left);
    took = now() - t0;
    check(r == 0 && idle.revents == 0, "ppoll with nothing to wake it answered", r);
    check(took >= 200 * MS && took < 3 * SEC, "a 200 ms ppoll timed out after (ns)", took);
    check(left.sec == 0 && left.nsec == 0, "a timed-out ppoll left (ns)", left.nsec);
    r = ppoll(&idle, 1, 0, 0);
    check(r == 0, "ppoll with a zero timeout answered", r);

    sc(SYS_close, q[0], 0, 0, 0, 0, 0);
    struct pollfd gone[2] = {{q[0], POLLIN, 0}, {q[1], POLLOUT, 0}};
    r = ppoll(gone, 2, 0, 0);
    check(r == 2, "ppoll on a closed descriptor and a reader-less write end answered", r);
    check(gone[0].revents == POLLNVAL, "a closed descriptor's revents were", gone[0].revents);
    check(gone[1].revents == (POLLOUT | POLLERR), "a write end with no reader: revents", gone[1].revents);
    sc(SYS_close, q[1], 0, 0, 0, 0, 0);
    say("linuxpoll: ppoll woke on a thread's write and at its close (POLLHUP), timed out on time, "
        "POLLNVAL and POLLERR\n");
}

/* --- pselect6, and a mask for the call alone ---------------------------------------------------- */

struct ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static volatile long handled;

static void on_usr1(int sig) {
    if (sig == SIGUSR1)
        handled++;
}

static long blocked_now(void) {
    u64 old = 0;
    sc(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&old, 8, 0, 0);
    return (long)old;
}

static void selecting(void) {
    int p[2];
    sc(SYS_pipe2, (long)p, 0, 0, 0, 0, 0);
    long pid = sc(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
    if (pid == 0) {
        sleep_ns(100 * MS);
        sc(SYS_write, p[1], (long)"f", 1, 0, 0, 0);
        sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
    }
    check(pid > 0, "fork answered", pid);
    u64 rd = 1UL << p[0];
    struct timespec t = {5, 0};
    long t0 = now();
    long r = sc(SYS_pselect6, p[0] + 1, (long)&rd, 0, 0, (long)&t, 0);
    long took = now() - t0;
    check(r == 1 && rd == 1UL << p[0], "pselect6 for the child's write answered", r);
    check(took >= 50 * MS && took < 3 * SEC, "pselect6 woke for the write after (ns)", took);
    u64 wr = 1UL << p[1];
    rd = 1UL << p[0];
    t.sec = 0;
    r = sc(SYS_pselect6, p[1] + 1, (long)&rd, (long)&wr, 0, (long)&t, 0);
    check(r == 2 && rd == 1UL << p[0] && wr == 1UL << p[1], "pselect6 for both ends answered", r);
    char b;
    sc(SYS_read, p[0], (long)&b, 1, 0, 0, 0);
    int status = 0;
    r = sc(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
    check(r == pid && status == 0, "the writing child ended with", status);
    u64 bad = 1UL << 13;
    r = sc(SYS_pselect6, 14, (long)&bad, 0, 0, (long)&t, 0);
    check(r == -EBADF, "pselect6 on a descriptor not open answered", r);
    sc(SYS_close, p[0], 0, 0, 0, 0, 0);
    sc(SYS_close, p[1], 0, 0, 0, 0, 0);

    struct ksigaction a = {(u64)on_usr1, SA_RESTORER, (u64)restorer, 0};
    r = sc(SYS_rt_sigaction, SIGUSR1, (long)&a, 0, 8, 0, 0);
    check(r == 0, "rt_sigaction answered", r);
    u64 usr1 = 1UL << (SIGUSR1 - 1);
    sc(SYS_rt_sigprocmask, SIG_BLOCK, (long)&usr1, 0, 8, 0, 0);
    long me = sc(SYS_getpid, 0, 0, 0, 0, 0, 0);

    /* Pending and blocked: the call's empty mask lets it in, so the call ends EINTR and the handler runs;
       after, SIGUSR1 is blocked again. */
    sc(SYS_kill, me, SIGUSR1, 0, 0, 0, 0);
    check(handled == 0, "a blocked signal was handled", handled);
    u64 none = 0;
    t.sec = 5;
    t0 = now();
    r = sc(SYS_ppoll, 0, 0, (long)&t, (long)&none, 8, 0);
    took = now() - t0;
    check(r == -EINTR, "ppoll with a mask letting a pending signal in answered", r);
    check(handled == 1, "the handler ran this many times in ppoll", handled);
    check(took < 3 * SEC, "ppoll's EINTR took (ns)", took);
    check((blocked_now() & usr1) != 0, "after ppoll SIGUSR1 is not blocked; the mask is", blocked_now());

    sc(SYS_kill, me, SIGUSR1, 0, 0, 0, 0);
    u64 data[2] = {(u64)&none, 8};
    r = sc(SYS_pselect6, 0, 0, 0, 0, (long)&t, (long)data);
    check(r == -EINTR, "pselect6 with a mask letting a pending signal in answered", r);
    check(handled == 2, "the handler ran this many times in all", handled);
    check((blocked_now() & usr1) != 0, "after pselect6 SIGUSR1 is not blocked; the mask is", blocked_now());
    sc(SYS_rt_sigprocmask, SIG_UNBLOCK, (long)&usr1, 0, 8, 0, 0);
    check(handled == 2, "a third SIGUSR1 came from nowhere", handled);
    say("linuxpoll: pselect6 woke on a child's write; a blocked SIGUSR1 came in through the call's mask alone: "
        "EINTR, handled, blocked again\n");
}

/* --- clocks ------------------------------------------------------------------------------------- */

static void clocks(void) {
    long t0 = now();
    sleep_ns(100 * MS);
    long t1 = now();
    check(t1 - t0 >= 100 * MS && t1 - t0 < 3 * SEC, "a 100 ms nanosleep took (ns)", t1 - t0);

    long target = t1 + 100 * MS;
    struct timespec at = {target / SEC, target % SEC};
    long r = sc(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, (long)&at, 0, 0, 0);
    long t2 = now();
    check(r == 0, "clock_nanosleep TIMER_ABSTIME answered", r);
    check(t2 >= target && t2 - target < 3 * SEC, "clock_nanosleep woke this long after its time (ns)", t2 - target);
    struct timespec rel = {0, 50 * MS};
    r = sc(SYS_clock_nanosleep, CLOCK_REALTIME, 0, (long)&rel, 0, 0, 0);
    check(r == 0, "a relative clock_nanosleep answered", r);

    long wall = clock_ns(CLOCK_REALTIME) / SEC;
    check(wall > 1577836800L, "CLOCK_REALTIME is before 2020, seconds", wall);
    struct timeval tv;
    r = sc(SYS_gettimeofday, (long)&tv, 0, 0, 0, 0, 0);
    check(r == 0 && tv.sec - wall >= 0 && tv.sec - wall <= 2, "gettimeofday's seconds differ by", tv.sec - wall);
    check(tv.usec >= 0 && tv.usec < 1000000, "gettimeofday's microseconds are", tv.usec);

    struct timespec res;
    r = sc(SYS_clock_getres, CLOCK_MONOTONIC, (long)&res, 0, 0, 0, 0);
    check(r == 0 && res.sec == 0 && res.nsec > 0 && res.nsec <= 1000, "CLOCK_MONOTONIC's resolution (ns)", res.nsec);
    r = sc(SYS_clock_getres, CLOCK_MONOTONIC_COARSE, (long)&res, 0, 0, 0, 0);
    check(r == 0 && res.sec == 0 && res.nsec > 0, "CLOCK_MONOTONIC_COARSE's resolution (ns)", res.nsec);
    r = sc(SYS_clock_gettime, 99, (long)&res, 0, 0, 0, 0);
    check(r == -EINVAL, "clock_gettime of clock 99 answered", r);
    say("linuxpoll: CLOCK_MONOTONIC advanced across nanosleep; clock_nanosleep TIMER_ABSTIME woke after its "
        "time; CLOCK_REALTIME is the RTC's date\n");
}

/* --- termios ------------------------------------------------------------------------------------ */

static int same(const struct ktermios *a, const struct ktermios *b) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (u64 i = 0; i < sizeof(struct ktermios); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void set(struct ktermios *t) {
    long r = sc(SYS_ioctl, 0, TCSETS, (long)t, 0, 0, 0);
    check(r == 0, "TCSETS answered", r);
}

static void terminal(void) {
    long r = sc(SYS_ioctl, 0, TCGETS, (long)&original, 0, 0, 0);
    check(r == 0, "TCGETS answered", r);
    check((original.lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG), "the console's lflag is", original.lflag);
    check((original.iflag & ICRNL) != 0, "the console's iflag is", original.iflag);
    check((original.oflag & (OPOST | ONLCR)) == (OPOST | ONLCR), "the console's oflag is", original.oflag);
    check(original.cc[VINTR] == 3 && original.cc[VERASE] == 127 && original.cc[VEOF] == 4 && original.cc[VMIN] == 1,
        "the console's VINTR is", original.cc[VINTR]);
    int p[2];
    sc(SYS_pipe2, (long)p, 0, 0, 0, 0, 0);
    struct ktermios t;
    r = sc(SYS_ioctl, p[0], TCGETS, (long)&t, 0, 0, 0);
    check(r == -ENOTTY, "TCGETS on a pipe answered", r);
    sc(SYS_close, p[0], 0, 0, 0, 0, 0);
    sc(SYS_close, p[1], 0, 0, 0, 0, 0);

    struct ktermios raw = original;
    raw.lflag &= ~(ICANON | ECHO);
    raw.cc[VMIN] = 3;
    raw.cc[VTIME] = 0;
    raw_mode = 1;
    set(&raw);
    struct ktermios back;
    sc(SYS_ioctl, 0, TCGETS, (long)&back, 0, 0, 0);
    check(same(&back, &raw), "TCGETS after TCSETS differs; lflag", back.lflag);
    say("linuxpoll: termios round trip; raw mode, waiting for three bytes\n");

    struct pollfd in = {0, POLLIN, 0};
    r = ppoll(&in, 1, 10 * SEC, 0);
    check(r == 1 && in.revents == POLLIN, "ppoll on the console in raw mode answered", r);
    char buf[16];
    r = sc(SYS_read, 0, (long)buf, sizeof(buf), 0, 0, 0);
    check(r == 3 && buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c', "the raw read answered", r);

    raw.lflag &= ~ISIG;
    raw.cc[VMIN] = 1;
    set(&raw);
    say("linuxpoll: raw read \"abc\" with no newline and no echo; ISIG off, Ctrl-C is a byte\n");
    r = sc(SYS_read, 0, (long)buf, sizeof(buf), 0, 0, 0);
    check(r == 1 && buf[0] == 3, "the read of Ctrl-C with ISIG off answered", r);

    raw.cc[VMIN] = 0;
    raw.cc[VTIME] = 2;
    set(&raw);
    long t0 = now();
    r = sc(SYS_read, 0, (long)buf, sizeof(buf), 0, 0, 0);
    long took = now() - t0;
    check(r == 0, "a VTIME read with nothing typed answered", r);
    check(took >= 150 * MS && took < 3 * SEC, "a VTIME of 0.2 s timed out after (ns)", took);

    set(&original);
    raw_mode = 0;
    sc(SYS_ioctl, 0, TCGETS, (long)&back, 0, 0, 0);
    check(same(&back, &original), "the restored settings differ; lflag", back.lflag);
    say("linuxpoll: read Ctrl-C as 0x03; VTIME timed out an idle read; termios restored\n");
}

/* The program starts here, with SP at argc (Linux's layout). */
__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "    mov x0, sp\n"
        "    b main_with\n");

void main_with(long *sp) {
    (void)sp;
    polling();
    selecting();
    clocks();
    terminal();
    sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
