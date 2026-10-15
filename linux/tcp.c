/* A static aarch64 Linux program, like poll.c: no C library, no keel header, only raw system calls by
   Linux's numbers. It is milestone 11b's proof of keel's TCP through Linux's socket ABI.

   linuxtcp echo <port> [<n>]
       Listens on <port> (any address), accepts one connection with accept4(SOCK_CLOEXEC), sends the
       line "keel ready", then echoes every byte until the peer half-closes, half-closes in turn, waits
       until every byte is acknowledged (no ACK may ever have come for a byte not yet sent), and reports
       how many bytes it echoed. With <n>, the connection drops every <n>th segment it would send
       (keel's own KEEL_DROP_EVERY option), and the run must show retransmissions, some after a
       timeout. scripts/tcp-echo-client.pl is the other end, on the host, through slirp.

   linuxtcp edges
       Client and server both on keel, over the loopback network 127.0.0.1: a refused connect, blocking
       and nonblocking; half-close and EPIPE after SHUT_WR; a peer closing mid-transfer (EOF, then EPIPE
       with SIGPIPE, and MSG_NOSIGNAL) and a peer resetting (ECONNRESET); poll's readiness as data, EOF
       and both shutdowns arrive; a full listen backlog; SO_RCVTIMEO, O_NONBLOCK; many connections in a
       row with the table and its frames back where they were; two connections at once through one
       server; a zero window held open with the persist timer, nothing sent past it; names, options,
       readv/writev; a signal ending a blocked read with EINTR.

   scripts/boot-test.sh builds it as it builds poll.c and puts it on the disk as /bin/linuxtcp. Bounds on
   times are wide: TCG is slow, and the point is that a wait ended for the right reason. */

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_fcntl 25
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_readv 65
#define SYS_writev 66
#define SYS_ppoll 73
#define SYS_exit_group 94
#define SYS_nanosleep 101
#define SYS_clock_gettime 113
#define SYS_kill 129
#define SYS_rt_sigaction 134
#define SYS_getpid 172
#define SYS_socket 198
#define SYS_bind 200
#define SYS_listen 201
#define SYS_accept 202
#define SYS_connect 203
#define SYS_getsockname 204
#define SYS_getpeername 205
#define SYS_sendto 206
#define SYS_recvfrom 207
#define SYS_setsockopt 208
#define SYS_getsockopt 209
#define SYS_shutdown 210
#define SYS_clone 220
#define SYS_accept4 242
#define SYS_wait4 260

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO 20
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define MSG_NOSIGNAL 0x4000
#define SHUT_WR 1
#define SOL_KEEL 0x6b65
#define KEEL_DROP_EVERY 1
#define KEEL_CONN_STATS 2
#define KEEL_TCP_TABLE 3

#define F_GETFD 1
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define O_NONBLOCK 0x800

#define POLLIN 0x1
#define POLLOUT 0x4
#define POLLERR 0x8
#define POLLHUP 0x10

#define SIGCHLD 17
#define SIGUSR1 10
#define SIGPIPE 13
#define SIG_IGN 1
#define SA_RESTORER 0x04000000

#define EINTR 4
#define EAGAIN 11
#define EPIPE 32
#define ECONNRESET 104
#define ENOTCONN 107
#define ECONNREFUSED 111
#define EINPROGRESS 115

#define CLOCK_MONOTONIC 1
#define MS 1000000L
#define SEC 1000000000L
#define LOOPBACK 0x7f000001u

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

/* clang may turn a loop or an initializer into these; there is no C library to supply them. */
void *memset(void *d, int c, u64 n) {
    volatile u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, u64 n) {
    volatile u8 *p = d;
    const u8 *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* The restorer a handler returns through: rt_sigreturn. */
__asm__(".text\n"
        ".global restorer\n"
        "restorer:\n"
        "    mov x8, #139\n"
        "    svc #0\n");
void restorer(void);

/* --- output ------------------------------------------------------------------------------------- */

static char out[512];
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
    put("linuxtcp: ");
    put(line);
    put("\n");
    flush();
}

static void fail(const char *what, long value) {
    put("linuxtcp: FAILED ");
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

/* --- small wrappers ----------------------------------------------------------------------------- */

struct timespec {
    long sec;
    long nsec;
};

static long now(void) {
    struct timespec t;
    sc(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&t, 0, 0, 0, 0);
    return t.sec * SEC + t.nsec;
}

static void nap(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * MS};
    sc(SYS_nanosleep, (long)&t, 0, 0, 0, 0, 0);
}

static long fork_now(void) {
    return sc(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
}

static int reap(long pid) {
    int status = 0;
    long r = sc(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
    check(r == pid, "wait4 answered", r);
    return status;
}

static void leave(int code) {
    sc(SYS_exit_group, code, 0, 0, 0, 0, 0);
}

struct sockaddr_in {
    u16 family;
    u16 port;
    u32 addr;
    u8 zero[8];
};

static u16 be16(u16 v) {
    return (u16)((v << 8) | (v >> 8));
}

static u32 be32(u32 v) {
    return (v << 24) | ((v << 8) & 0xff0000) | ((v >> 8) & 0xff00) | (v >> 24);
}

static struct sockaddr_in address(u32 ip, u16 port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.family = AF_INET;
    a.port = be16(port);
    a.addr = be32(ip);
    return a;
}

static long set_int(long fd, long level, long option, int value) {
    return sc(SYS_setsockopt, fd, level, option, (long)&value, 4, 0);
}

static int get_int(long fd, long level, long option) {
    int value = -1;
    u32 len = 4;
    long r = sc(SYS_getsockopt, fd, level, option, (long)&value, (long)&len, 0);
    check(r == 0 && len == 4, "getsockopt answered", r);
    return value;
}

static long listener(u32 ip, u16 port, int backlog) {
    long s = sc(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    check(s >= 0, "socket answered", s);
    check(set_int(s, SOL_SOCKET, SO_REUSEADDR, 1) == 0, "SO_REUSEADDR refused on", s);
    struct sockaddr_in a = address(ip, port);
    long r = sc(SYS_bind, s, (long)&a, sizeof(a), 0, 0, 0);
    check(r == 0, "bind answered", r);
    r = sc(SYS_listen, s, backlog, 0, 0, 0, 0);
    check(r == 0, "listen answered", r);
    return s;
}

static long dial(u16 port) {
    long s = sc(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    check(s >= 0, "socket answered", s);
    struct sockaddr_in a = address(LOOPBACK, port);
    long r = sc(SYS_connect, s, (long)&a, sizeof(a), 0, 0, 0);
    check(r == 0, "connect to 127.0.0.1 answered", r);
    return s;
}

static long take(long l) {
    long a = sc(SYS_accept, l, 0, 0, 0, 0, 0);
    check(a >= 0, "accept answered", a);
    return a;
}

static void send_all(long fd, const u8 *p, long n) {
    while (n > 0) {
        long r = sc(SYS_write, fd, (long)p, n, 0, 0, 0);
        check(r > 0, "a write answered", r);
        p += r;
        n -= r;
    }
}

/* Read until the end of the stream; answers how many bytes, at most `room` kept. */
static long read_to_end(long fd, u8 *p, long room) {
    long got = 0;
    for (;;) {
        u8 scratch[512];
        u8 *into = got < room ? p + got : scratch;
        long ask = got < room ? room - got : (long)sizeof(scratch);
        long r = sc(SYS_read, fd, (long)into, ask, 0, 0, 0);
        check(r >= 0, "a read answered", r);
        if (r == 0)
            return got;
        got += r;
    }
}

static int same(const u8 *a, const char *b, long n) {
    for (long i = 0; i < n; i++)
        if (a[i] != (u8)b[i])
            return 0;
    return 1;
}

struct pollfd {
    int fd;
    short events;
    short revents;
};

/* ppoll on one descriptor for up to `ms` (-1: no limit); answers revents, 0 on a timeout. */
static int poll_one(long fd, short events, long ms) {
    struct pollfd p = {(int)fd, events, 0};
    struct timespec t = {ms / 1000, (ms % 1000) * MS};
    long r = sc(SYS_ppoll, (long)&p, 1, ms < 0 ? 0 : (long)&t, 0, 8, 0);
    check(r >= 0, "ppoll answered", r);
    return r == 0 ? 0 : p.revents;
}

struct conn_stats {
    u64 retransmits, timeouts, fast_retransmits, probes, dropped, beyond_window, unacked, acks_ahead;
};

struct tcp_table {
    u64 live, time_wait, frames, retransmits, bad_checksums, syns_dropped;
};

static struct conn_stats conn_stats(long fd) {
    struct conn_stats s;
    u32 len = sizeof(s);
    long r = sc(SYS_getsockopt, fd, SOL_KEEL, KEEL_CONN_STATS, (long)&s, (long)&len, 0);
    check(r == 0 && len == sizeof(s), "KEEL_CONN_STATS answered", r);
    return s;
}

static struct tcp_table table(long fd) {
    struct tcp_table t;
    u32 len = sizeof(t);
    long r = sc(SYS_getsockopt, fd, SOL_KEEL, KEEL_TCP_TABLE, (long)&t, (long)&len, 0);
    check(r == 0 && len == sizeof(t), "KEEL_TCP_TABLE answered", r);
    return t;
}

struct ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static volatile int signals_seen;

static void counting(int sig) {
    (void)sig;
    signals_seen++;
}

static void on_signal(int sig, u64 handler) {
    struct ksigaction a = {handler, SA_RESTORER, (u64)restorer, 0};
    long r = sc(SYS_rt_sigaction, sig, (long)&a, 0, 8, 0, 0);
    check(r == 0, "rt_sigaction answered", r);
}

/* A byte of a deterministic stream: what position `i` of stream `seed` holds. */
static u8 pattern(u64 seed, u64 i) {
    u64 x = (i + 1) * 0x9e3779b97f4a7c15ul ^ (seed * 0xbf58476d1ce4e5b9ul);
    x ^= x >> 29;
    return (u8)(x ^ (x >> 11));
}

/* --- echo, for the host ------------------------------------------------------------------------- */

static u8 big[16384];

static void echo(long port, long drop) {
    long l = listener(0, (u16)port, 4);
    put("linuxtcp: listening on port ");
    put_number(port);
    put("\n");
    flush();
    long c = sc(SYS_accept4, l, 0, 0, SOCK_CLOEXEC, 0, 0);
    check(c >= 0, "accept4 answered", c);
    check(sc(SYS_fcntl, c, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC, "accept4's SOCK_CLOEXEC is not FD_CLOEXEC on", c);
    if (drop > 0)
        check(set_int(c, SOL_KEEL, KEEL_DROP_EVERY, (int)drop) == 0, "KEEL_DROP_EVERY refused on", c);
    send_all(c, (const u8 *)"keel ready\n", 11);
    long total = 0;
    for (;;) {
        long n = sc(SYS_read, c, (long)big, sizeof(big), 0, 0, 0);
        check(n >= 0, "an echo read answered", n);
        if (n == 0)
            break;
        send_all(c, big, n);
        total += n;
    }
    check(sc(SYS_shutdown, c, SHUT_WR, 0, 0, 0, 0) == 0, "shutdown(SHUT_WR) refused on", c);
    /* The peer half-closed first, so this read ends at once. */
    long r = sc(SYS_read, c, (long)big, sizeof(big), 0, 0, 0);
    check(r == 0, "the read after the echo answered", r);
    /* Wait until every byte and the FIN are acknowledged, so every retransmission is counted and a
       connection that stopped making progress is caught here rather than as a short echo on the host. */
    long began = now();
    struct conn_stats s = conn_stats(c);
    while (s.unacked != 0 && now() - began < 30 * SEC) {
        nap(10);
        s = conn_stats(c);
    }
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    put("linuxtcp: echoed ");
    put_number(total);
    put(" bytes");
    if (drop > 0) {
        put(" with every ");
        put_number(drop);
        put("th segment dropped: dropped ");
        put_number((long)s.dropped);
        put(", retransmitted ");
        put_number((long)s.retransmits);
        put(" (");
        put_number((long)s.timeouts);
        put(" after a timeout, ");
        put_number((long)s.fast_retransmits);
        put(" fast)");
    }
    put("\n");
    flush();
    check(s.unacked == 0, "bytes still unacknowledged 30 s after the echo:", (long)s.unacked);
    check(s.acks_ahead == 0, "ACKs came for bytes never sent:", (long)s.acks_ahead);
    if (drop > 0) {
        check(s.dropped > 0, "nothing was dropped:", (long)s.dropped);
        check(s.retransmits > 0, "nothing was retransmitted:", (long)s.retransmits);
        check(s.timeouts > 0, "no retransmission timed out:", (long)s.timeouts);
    }
}

/* --- edges, on the loopback network ------------------------------------------------------------- */

static void refused(void) {
    long s = sc(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    struct sockaddr_in a = address(LOOPBACK, 9);
    long r = sc(SYS_connect, s, (long)&a, sizeof(a), 0, 0, 0);
    check(r == -ECONNREFUSED, "connect to a closed port answered", r);
    sc(SYS_close, s, 0, 0, 0, 0, 0);

    s = sc(SYS_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    check((sc(SYS_fcntl, s, F_GETFL, 0, 0, 0, 0) & O_NONBLOCK) != 0, "SOCK_NONBLOCK is not O_NONBLOCK on", s);
    r = sc(SYS_connect, s, (long)&a, sizeof(a), 0, 0, 0);
    check(r == -EINPROGRESS, "a nonblocking connect answered", r);
    /* Linux's answer for a connection that failed: POLLERR and POLLHUP (and not POLLOUT: tcp_done shut
       both directions). */
    int ev = poll_one(s, POLLOUT, 5000);
    check((ev & POLLERR) && (ev & POLLHUP), "the refused connect's revents were", ev);
    check(get_int(s, SOL_SOCKET, SO_ERROR) == ECONNREFUSED, "SO_ERROR after the refusal was",
        get_int(s, SOL_SOCKET, SO_ERROR));
    sc(SYS_close, s, 0, 0, 0, 0, 0);
    say("connect to a closed port: ECONNREFUSED; nonblocking, EINPROGRESS then POLLERR|POLLHUP and SO_ERROR");
}

static void half_close(void) {
    long l = listener(LOOPBACK, 9101, 4);
    long pid = fork_now();
    if (pid == 0) {
        struct ksigaction ign = {SIG_IGN, SA_RESTORER, (u64)restorer, 0};
        sc(SYS_rt_sigaction, SIGPIPE, (long)&ign, 0, 8, 0, 0);
        long c = dial(9101);
        send_all(c, (const u8 *)"ping", 4);
        check(sc(SYS_shutdown, c, SHUT_WR, 0, 0, 0, 0) == 0, "shutdown(SHUT_WR) refused on", c);
        long r = sc(SYS_write, c, (long)"x", 1, 0, 0, 0);
        check(r == -EPIPE, "a write after SHUT_WR answered", r);
        u8 got[16];
        long n = read_to_end(c, got, sizeof(got));
        check(n == 4 && same(got, "pong", 4), "the client read back this many bytes", n);
        leave(0);
    }
    long a = take(l);
    u8 got[16];
    long n = read_to_end(a, got, sizeof(got));
    check(n == 4 && same(got, "ping", 4), "the server read this many bytes before the end", n);
    send_all(a, (const u8 *)"pong", 4);
    sc(SYS_close, a, 0, 0, 0, 0, 0);
    check(reap(pid) == 0, "the half-closing client's status was", 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("half-close: ping, end of stream, pong, end of stream; a write after SHUT_WR is EPIPE");
}

static void peer_gone(void) {
    long l = listener(LOOPBACK, 9102, 4);
    long pid = fork_now();
    if (pid == 0) {
        long a = take(l);
        send_all(a, (const u8 *)"bye", 3);
        leave(0);
    }
    long c = dial(9102);
    u8 got[16];
    long n = read_to_end(c, got, sizeof(got));
    check(n == 3 && same(got, "bye", 3), "the read before the peer's close answered", n);
    check(reap(pid) == 0, "the closing peer's status was", 0);
    /* The peer's program has closed its end: what is written now is answered with an RST. */
    long r = sc(SYS_write, c, (long)"x", 1, 0, 0, 0);
    check(r == 1, "the first write after the peer's close answered", r);
    nap(200);
    on_signal(SIGPIPE, (u64)counting);
    signals_seen = 0;
    r = sc(SYS_write, c, (long)"y", 1, 0, 0, 0);
    check(r == -EPIPE, "a write after the peer's RST answered", r);
    check(signals_seen == 1, "SIGPIPE handled this many times after EPIPE:", signals_seen);
    r = sc(SYS_sendto, c, (long)"z", 1, MSG_NOSIGNAL, 0, 0);
    check(r == -EPIPE, "send with MSG_NOSIGNAL answered", r);
    check(signals_seen == 1, "SIGPIPE came with MSG_NOSIGNAL; handled", signals_seen);
    sc(SYS_close, c, 0, 0, 0, 0, 0);

    /* A peer that closes with data it never read resets the connection. */
    pid = fork_now();
    if (pid == 0) {
        (void)take(l);
        nap(300);
        leave(0);
    }
    c = dial(9102);
    send_all(c, (const u8 *)"unread", 6);
    r = sc(SYS_read, c, (long)got, sizeof(got), 0, 0, 0);
    check(r == -ECONNRESET, "the read after the peer's reset answered", r);
    r = sc(SYS_read, c, (long)got, sizeof(got), 0, 0, 0);
    check(r == 0, "the read after ECONNRESET answered", r);
    check(reap(pid) == 0, "the resetting peer's status was", 0);
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("peer closed: its bytes, the end, then EPIPE with SIGPIPE, without it for MSG_NOSIGNAL; "
        "closed unread: ECONNRESET, then the end");
}

static void readiness(void) {
    long l = listener(LOOPBACK, 9103, 4);
    long c = dial(9103);
    long a = take(l);
    int ev = poll_one(c, POLLIN | POLLOUT, 0);
    check(ev == POLLOUT, "a quiet connection's revents were", ev);
    long pid = fork_now();
    if (pid == 0) {
        nap(100);
        send_all(a, (const u8 *)"!", 1);
        leave(0);
    }
    long t0 = now();
    ev = poll_one(c, POLLIN, 3000);
    long took = now() - t0;
    check(ev == POLLIN, "ppoll for the byte another process sent answered", ev);
    check(took < 2 * SEC, "the byte's POLLIN came after (ns)", took);
    u8 b[4];
    check(sc(SYS_read, c, (long)b, 4, 0, 0, 0) == 1 && b[0] == '!', "the polled byte was", b[0]);
    check(reap(pid) == 0, "the writer's status was", 0);
    sc(SYS_close, a, 0, 0, 0, 0, 0);
    ev = poll_one(c, POLLIN, 3000);
    check(ev == POLLIN, "ppoll after the peer's close answered", ev);
    check(sc(SYS_read, c, (long)b, 4, 0, 0, 0) == 0, "the read at the end answered", 1);
    check(sc(SYS_shutdown, c, SHUT_WR, 0, 0, 0, 0) == 0, "shutdown(SHUT_WR) refused on", c);
    ev = poll_one(c, POLLIN, 0);
    check((ev & POLLHUP) != 0, "both directions shut, revents were", ev);
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("ppoll: POLLOUT alone while quiet, POLLIN for a byte another process sent, POLLIN at the end, "
        "POLLHUP once both directions are shut");
}

static void backlog(void) {
    long l = listener(LOOPBACK, 9104, 1);
    long before = (long)table(l).syns_dropped;
    long first = dial(9104);
    long second = sc(SYS_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    struct sockaddr_in a = address(LOOPBACK, 9104);
    long r = sc(SYS_connect, second, (long)&a, sizeof(a), 0, 0, 0);
    check(r == -EINPROGRESS, "the second connect answered", r);
    int ev = poll_one(second, POLLOUT, 400);
    check(ev == 0, "with the backlog full the second connect's revents were", ev);
    check((long)table(l).syns_dropped > before, "no SYN was dropped for the full backlog:", before);
    long one = take(l);
    ev = poll_one(second, POLLOUT, 8000);
    check(ev == POLLOUT, "once accepted, the second connect's revents were", ev);
    check(get_int(second, SOL_SOCKET, SO_ERROR) == 0, "the second connect's SO_ERROR was",
        get_int(second, SOL_SOCKET, SO_ERROR));
    long two = take(l);
    long fds[5] = {l, first, second, one, two};
    for (int i = 0; i < 5; i++)
        sc(SYS_close, fds[i], 0, 0, 0, 0, 0);
    say("listen backlog 1: a second SYN dropped while one waits, the connection made once it was accepted");
}

static void timeouts(void) {
    long l = listener(LOOPBACK, 9105, 4);
    struct timespec tv = {0, 200000};   /* struct timeval: 0 s, 200,000 us */
    check(sc(SYS_setsockopt, l, SOL_SOCKET, SO_RCVTIMEO, (long)&tv, sizeof(tv), 0) == 0, "SO_RCVTIMEO refused", 0);
    long t0 = now();
    long r = sc(SYS_accept, l, 0, 0, 0, 0, 0);
    long took = now() - t0;
    check(r == -EAGAIN, "accept past SO_RCVTIMEO answered", r);
    check(took >= 150 * MS && took < 5 * SEC, "accept's SO_RCVTIMEO of 0.2 s took (ns)", took);
    long c = dial(9105);
    long a = take(l);
    check(sc(SYS_setsockopt, c, SOL_SOCKET, SO_RCVTIMEO, (long)&tv, sizeof(tv), 0) == 0, "SO_RCVTIMEO refused", 1);
    u8 b[4];
    t0 = now();
    r = sc(SYS_read, c, (long)b, 4, 0, 0, 0);
    took = now() - t0;
    check(r == -EAGAIN, "a read past SO_RCVTIMEO answered", r);
    check(took >= 150 * MS && took < 5 * SEC, "the read's SO_RCVTIMEO of 0.2 s took (ns)", took);
    long fl = sc(SYS_fcntl, a, F_GETFL, 0, 0, 0, 0);
    check(sc(SYS_fcntl, a, F_SETFL, fl | O_NONBLOCK, 0, 0, 0) == 0, "F_SETFL O_NONBLOCK refused on", a);
    r = sc(SYS_read, a, (long)b, 4, 0, 0, 0);
    check(r == -EAGAIN, "a nonblocking read with nothing there answered", r);
    sc(SYS_close, a, 0, 0, 0, 0, 0);
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("SO_RCVTIMEO: accept and read give up with EAGAIN after 0.2 s; O_NONBLOCK: EAGAIN at once");
}

static void many(void) {
    long l = listener(LOOPBACK, 9106, 4);
    struct tcp_table start = table(l);
    long first_fd = -1;
    for (int i = 0; i < 40; i++) {
        long c = dial(9106);
        if (first_fd < 0)
            first_fd = c;
        check(c == first_fd, "a connection's descriptor moved up to", c);
        long a = take(l);
        u8 msg[100], got[100];
        for (int k = 0; k < 100; k++)
            msg[k] = pattern((u64)i, (u64)k);
        send_all(c, msg, 100);
        long n = 0;
        while (n < 100) {
            long r = sc(SYS_read, a, (long)(got + n), 100 - n, 0, 0, 0);
            check(r > 0, "a read in the loop answered", r);
            n += r;
        }
        for (int k = 0; k < 100; k++)
            check(got[k] == msg[k], "a byte in the loop differs at", k);
        sc(SYS_close, c, 0, 0, 0, 0, 0);
        sc(SYS_close, a, 0, 0, 0, 0, 0);
    }
    /* The last closes finish in the stack's own time; give them a moment. */
    struct tcp_table end = table(l);
    for (int tries = 0; tries < 100 && (end.live != start.live || end.frames != start.frames); tries++) {
        nap(20);
        end = table(l);
    }
    check(end.live == start.live, "connections still live after 40 in a row:", (long)end.live - (long)start.live);
    check(end.frames == start.frames, "frames still held after 40 in a row:", (long)end.frames - (long)start.frames);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    put("linuxtcp: 40 connections in a row: the same descriptor each time, the table and its frames back where they were (");
    put_number((long)end.time_wait);
    put(" in TIME_WAIT)\n");
    flush();
}

/* One side of a two-way stream through ppoll and nonblocking calls: send `n` bytes of stream `seed` and
   check that the same `n` come back. */
static void pump(long fd, u64 seed, long n) {
    long fl = sc(SYS_fcntl, fd, F_GETFL, 0, 0, 0, 0);
    sc(SYS_fcntl, fd, F_SETFL, fl | O_NONBLOCK, 0, 0, 0);
    long sent = 0, got = 0;
    u8 buf[2048];
    while (got < n) {
        short want = POLLIN | (sent < n ? POLLOUT : 0);
        int ev = poll_one(fd, want, 10000);
        check(ev != 0, "a stream stalled; bytes back so far", got);
        if ((ev & POLLOUT) && sent < n) {
            long k = n - sent < (long)sizeof(buf) ? n - sent : (long)sizeof(buf);
            for (long i = 0; i < k; i++)
                buf[i] = pattern(seed, (u64)(sent + i));
            long r = sc(SYS_write, fd, (long)buf, k, 0, 0, 0);
            check(r > 0 || r == -EAGAIN, "a nonblocking write answered", r);
            if (r > 0)
                sent += r;
        }
        if (ev & POLLIN) {
            long r = sc(SYS_read, fd, (long)buf, sizeof(buf), 0, 0, 0);
            check(r > 0 || r == -EAGAIN, "a nonblocking read answered", r);
            for (long i = 0; i < r; i++)
                check(buf[i] == pattern(seed, (u64)(got + i)), "an echoed byte differs at", got + i);
            if (r > 0)
                got += r;
        }
    }
}

static void two_at_once(void) {
    long l = listener(LOOPBACK, 9107, 4);
    long server = fork_now();
    if (server == 0) {
        long conn[2] = {take(l), take(l)};
        u8 buf[2][2048];
        long have[2] = {0, 0}, off[2] = {0, 0};
        int open[2] = {1, 1};
        for (int i = 0; i < 2; i++) {
            long fl = sc(SYS_fcntl, conn[i], F_GETFL, 0, 0, 0, 0);
            sc(SYS_fcntl, conn[i], F_SETFL, fl | O_NONBLOCK, 0, 0, 0);
        }
        while (open[0] || open[1] || have[0] > off[0] || have[1] > off[1]) {
            struct pollfd p[2];
            for (int i = 0; i < 2; i++) {
                p[i].fd = (int)conn[i];
                p[i].events = (short)((have[i] > off[i] ? POLLOUT : 0) | (open[i] && have[i] == off[i] ? POLLIN : 0));
                p[i].revents = 0;
            }
            struct timespec t = {10, 0};
            long r = sc(SYS_ppoll, (long)p, 2, (long)&t, 0, 8, 0);
            check(r > 0, "the server's ppoll answered", r);
            for (int i = 0; i < 2; i++) {
                if ((p[i].revents & POLLOUT) && have[i] > off[i]) {
                    long w = sc(SYS_write, conn[i], (long)(buf[i] + off[i]), have[i] - off[i], 0, 0, 0);
                    check(w > 0 || w == -EAGAIN, "the server's write answered", w);
                    if (w > 0)
                        off[i] += w;
                } else if ((p[i].revents & (POLLIN | POLLHUP)) && open[i] && have[i] == off[i]) {
                    long g = sc(SYS_read, conn[i], (long)buf[i], sizeof(buf[i]), 0, 0, 0);
                    check(g >= 0 || g == -EAGAIN, "the server's read answered", g);
                    if (g == 0)
                        open[i] = 0;
                    if (g > 0) {
                        have[i] = g;
                        off[i] = 0;
                    }
                }
            }
        }
        leave(0);
    }
    long clients[2];
    for (int i = 0; i < 2; i++) {
        clients[i] = fork_now();
        if (clients[i] == 0) {
            long c = dial(9107);
            pump(c, (u64)(100 + i), 262144);
            check(sc(SYS_shutdown, c, SHUT_WR, 0, 0, 0, 0) == 0, "a client's shutdown refused on", c);
            leave(0);
        }
    }
    for (int i = 0; i < 2; i++)
        check(reap(clients[i]) == 0, "a concurrent client's status was", i);
    check(reap(server) == 0, "the concurrent server's status was", 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("two connections at once through one server: 256 KiB each way on each, every byte back in order");
}

static void zero_window(void) {
    long l = listener(LOOPBACK, 9108, 4);
    long c = dial(9108);
    long a = take(l);
    long fl = sc(SYS_fcntl, c, F_GETFL, 0, 0, 0, 0);
    sc(SYS_fcntl, c, F_SETFL, fl | O_NONBLOCK, 0, 0, 0);
    long in = 0;
    u8 buf[4096];
    for (;;) {
        for (long i = 0; i < (long)sizeof(buf); i++)
            buf[i] = pattern(7, (u64)(in + i));
        long r = sc(SYS_write, c, (long)buf, sizeof(buf), 0, 0, 0);
        if (r == -EAGAIN) {
            /* The rings fill as the segments are acknowledged; look once more after a moment. */
            nap(100);
            r = sc(SYS_write, c, (long)buf, sizeof(buf), 0, 0, 0);
            if (r == -EAGAIN)
                break;
        }
        check(r > 0, "a write into the closing window answered", r);
        in += r;
        check(in < 1000000, "the writer was never stopped; bytes in:", in);
    }
    check(in >= 32768, "the writer stopped early; bytes in:", in);
    nap(800);
    long got = 0;
    while (got < in) {
        long r = sc(SYS_read, a, (long)buf, sizeof(buf), 0, 0, 0);
        check(r > 0, "the slow reader's read answered", r);
        for (long i = 0; i < r; i++)
            check(buf[i] == pattern(7, (u64)(got + i)), "a byte through the zero window differs at", got + i);
        got += r;
    }
    struct conn_stats sender = conn_stats(c);
    struct conn_stats receiver = conn_stats(a);
    check(receiver.beyond_window == 0, "segments arrived past the advertised window:", (long)receiver.beyond_window);
    check(sender.probes > 0, "no zero-window probe was sent; probes", (long)sender.probes);
    sc(SYS_close, a, 0, 0, 0, 0, 0);
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("zero window: the writer stopped with both rings full, the persist timer probed while the reader "
        "slept, every byte read back in order and none sent past the window");
}

struct iovec {
    const void *base;
    u64 len;
};

static void names_and_options(void) {
    long l = listener(LOOPBACK, 9109, 4);
    long c = dial(9109);
    struct sockaddr_in peer;
    u32 len = sizeof(peer);
    long a = sc(SYS_accept4, l, (long)&peer, (long)&len, SOCK_CLOEXEC, 0, 0);
    check(a >= 0, "accept4 answered", a);
    check(len == sizeof(peer) && peer.addr == be32(LOOPBACK), "accept4's address was not 127.0.0.1; length", len);
    check(sc(SYS_fcntl, a, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC, "accept4's SOCK_CLOEXEC is not FD_CLOEXEC on", a);
    struct sockaddr_in mine, theirs;
    len = sizeof(mine);
    check(sc(SYS_getsockname, c, (long)&mine, (long)&len, 0, 0, 0) == 0, "getsockname refused on", c);
    len = sizeof(theirs);
    check(sc(SYS_getpeername, c, (long)&theirs, (long)&len, 0, 0, 0) == 0, "getpeername refused on", c);
    check(mine.addr == be32(LOOPBACK) && be16(mine.port) >= 49152, "the client's own port was", be16(mine.port));
    check(be16(theirs.port) == 9109, "the client's peer port was", be16(theirs.port));
    check(peer.port == mine.port, "the server saw the client's port as", be16(peer.port));
    check(get_int(c, SOL_SOCKET, SO_TYPE) == SOCK_STREAM, "SO_TYPE was", get_int(c, SOL_SOCKET, SO_TYPE));
    check(set_int(c, IPPROTO_TCP, TCP_NODELAY, 1) == 0 && get_int(c, IPPROTO_TCP, TCP_NODELAY) == 1,
        "TCP_NODELAY did not stick", 0);
    check(set_int(c, SOL_SOCKET, SO_KEEPALIVE, 1) == 0 && get_int(c, SOL_SOCKET, SO_KEEPALIVE) == 1,
        "SO_KEEPALIVE did not stick", 0);
    struct iovec two[2] = {{"ab", 2}, {"cd", 2}};
    check(sc(SYS_writev, c, (long)two, 2, 0, 0, 0) == 4, "writev answered", 0);
    u8 x[2], y[2];
    struct iovec back[2] = {{x, 2}, {y, 2}};
    long n = 0;
    while (n < 4) {
        long r = sc(SYS_readv, a, (long)back, 2, 0, 0, 0);
        check(r > 0, "readv answered", r);
        n += r;
        if (n < 4) {
            /* A short readv: take the rest into the second buffer. */
            back[0].len = 0;
            back[1].base = y + (n - 2 > 0 ? n - 2 : 0);
            back[1].len = (u64)(4 - n);
        }
    }
    check(same(x, "ab", 2) && same(y, "cd", 2), "readv's bytes differ", 0);
    long s = sc(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    len = sizeof(theirs);
    long r = sc(SYS_getpeername, s, (long)&theirs, (long)&len, 0, 0, 0);
    check(r == -ENOTCONN, "getpeername on an unconnected socket answered", r);
    long fds[4] = {s, a, c, l};
    for (int i = 0; i < 4; i++)
        sc(SYS_close, fds[i], 0, 0, 0, 0, 0);
    say("names: getsockname, getpeername and accept4's address agree; SO_TYPE, TCP_NODELAY, SO_KEEPALIVE; "
        "writev and readv; ENOTCONN unconnected");
}

static void interrupted(void) {
    long l = listener(LOOPBACK, 9110, 4);
    long c = dial(9110);
    long a = take(l);
    on_signal(SIGUSR1, (u64)counting);
    signals_seen = 0;
    long me = sc(SYS_getpid, 0, 0, 0, 0, 0, 0);
    long pid = fork_now();
    if (pid == 0) {
        nap(150);
        sc(SYS_kill, me, SIGUSR1, 0, 0, 0, 0);
        leave(0);
    }
    u8 b[4];
    long r = sc(SYS_read, c, (long)b, 4, 0, 0, 0);
    check(r == -EINTR, "a read a signal interrupted answered", r);
    check(signals_seen == 1, "the handler ran this many times:", signals_seen);
    check(reap(pid) == 0, "the signalling child's status was", 0);
    sc(SYS_close, a, 0, 0, 0, 0, 0);
    sc(SYS_close, c, 0, 0, 0, 0, 0);
    sc(SYS_close, l, 0, 0, 0, 0, 0);
    say("a signal ends a blocked read with EINTR, its handler run");
}

static long number(const char *s) {
    long n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

static int is(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The program starts here, with SP at argc (Linux's layout). */
__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "    mov x0, sp\n"
        "    b main_with\n");

void main_with(long *sp) {
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    if (argc >= 3 && is(argv[1], "echo")) {
        echo(number(argv[2]), argc >= 4 ? number(argv[3]) : 0);
    } else if (argc >= 2 && is(argv[1], "edges")) {
        refused();
        half_close();
        peer_gone();
        readiness();
        backlog();
        timeouts();
        many();
        two_at_once();
        zero_window();
        names_and_options();
        interrupted();
    } else {
        say("usage: linuxtcp echo <port> [<drop every>] | linuxtcp edges");
        leave(2);
    }
    leave(0);
}
