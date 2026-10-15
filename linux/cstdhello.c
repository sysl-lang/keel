/* An ordinary C program, written against the C library and nothing of keel's: cstd (a C library
   written in sysl) supplies the headers, the C runtime that calls main, and every function below,
   and its system calls are Linux aarch64's. scripts/boot-test.sh compiles it with clang against
   cstd's include/ and links it with ld.lld against the archive `sysl build-c` makes of cstd's Linux
   half; it is /bin/cstdhello on the disk, run from the shell as `cstdhello one two`.

   Each line is printed only after what it claims was checked; a check that fails prints a line
   starting "cstdhello: FAILED" and exits 1. */

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *what) {
    printf("cstdhello: FAILED %s (errno %d: %s)\n", what, errno, strerror(errno));
    exit(1);
}

/* atexit: three handlers, which must run last-registered first. */
static char ran[4];
static int ran_count;

static void handler_a(void) {
    ran[ran_count++] = 'a';
    if (ran_count == 3 && strcmp(ran, "cba") == 0)
        printf("cstdhello: atexit: 3 handlers ran last-registered first\n");
    else
        printf("cstdhello: FAILED atexit order %s\n", ran);
}
static void handler_b(void) { ran[ran_count++] = 'b'; }
static void handler_c(void) { ran[ran_count++] = 'c'; }

static void check_printf(void) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%05.1f|%-4s|%+d", 2.25, "ab", 7);
    if (n != 13 ||strcmp(buf, "002.2|ab  |+7") != 0)
        fail("snprintf");
    printf("cstdhello: printf: %d %#x '%s' %.3f %.2e %g %c%%\n", -42, 42, "keel", 3.14159, 0.0025, 1e21, '!');
}

static void check_args(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[0], "cstdhello") != 0 || argv[3] != NULL)
        fail("argv");
    const char *greeting = NULL;
    int count = 0;
    for (char **e = environ; *e != NULL; e++, count++)
        if (strncmp(*e, "GREETING=", 9) == 0)
            greeting = *e + 9;
    if (greeting == NULL || strcmp(greeting, "hi") != 0)
        fail("environ");
    /* The environment vector starts one past argv's null, where the kernel laid it. */
    if (environ != argv + argc + 1)
        fail("environ's place");
    printf("cstdhello: argv: %d %s %s %s; GREETING=%s among %d environment strings\n", argc, argv[0], argv[1],
           argv[2], greeting, count);
}

/* malloc: 3000 blocks of 1 to 500 bytes, each filled with its own pattern; every third grown by
   realloc, every block checked; all freed in a scrambled order; then one block nearly as large as
   all of them together must come from the freed memory without the break moving — which only
   holds if freed neighbours were merged. */
#define BLOCKS 3000

static unsigned char *blocks[BLOCKS];
static size_t sizes[BLOCKS];

static void fill(unsigned char *p, size_t n, int seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(seed + i * 7);
}

static int holds(const unsigned char *p, size_t n, int seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(seed + i * 7))
            return 0;
    return 1;
}

static void check_malloc(void) {
    size_t total = 0;
    int grown = 0;
    for (int i = 0; i < BLOCKS; i++) {
        sizes[i] = (size_t)(i * 37 % 500) + 1;
        blocks[i] = malloc(sizes[i]);
        if (blocks[i] == NULL || ((unsigned long)blocks[i] & 15) != 0)
            fail("malloc");
        fill(blocks[i], sizes[i], i);
        if (i % 3 == 0) {
            size_t bigger = sizes[i] * 2 + 40;
            unsigned char *q = realloc(blocks[i], bigger);
            if (q == NULL || !holds(q, sizes[i], i))
                fail("realloc");
            blocks[i] = q;
            sizes[i] = bigger;
            fill(q, bigger, i);
            grown++;
        }
        total += sizes[i];
    }
    for (int i = 0; i < BLOCKS; i++)
        if (!holds(blocks[i], sizes[i], i))
            fail("a block lost its bytes");
    for (int k = 0; k < BLOCKS; k++)
        free(blocks[k * 1999 % BLOCKS]);

    void *brk_before = sbrk(0);
    size_t big = total * 9 / 10;
    unsigned char *p = malloc(big);
    if (p == NULL)
        fail("the big malloc");
    fill(p, big, 5);
    if (!holds(p, big, 5))
        fail("the big block");
    if (sbrk(0) != brk_before)
        fail("the freed blocks were not merged: the big block moved the break");
    free(p);

    int *zeros = calloc(1000, sizeof(int));
    for (int i = 0; i < 1000; i++)
        if (zeros == NULL || zeros[i] != 0)
            fail("calloc");
    free(zeros);
    printf("cstdhello: malloc: %d blocks checked, %d grown by realloc, a %zu-byte block reused the freed run\n",
           BLOCKS, grown, big);
}

static void check_files(void) {
    static const char text[] = "written by cstd\n";
    int fd = open("/m9/cstd.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        fail("open for writing");
    if (write(fd, text, sizeof text - 1) != (ssize_t)(sizeof text - 1))
        fail("write");
    if (close(fd) != 0)
        fail("close");

    char back[32];
    fd = open("/m9/cstd.txt", O_RDONLY);
    if (fd < 0)
        fail("open for reading");
    ssize_t n = read(fd, back, sizeof back);
    if (n != (ssize_t)(sizeof text - 1) || memcmp(back, text, (size_t)n) != 0)
        fail("read");
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != n || !S_ISREG(st.st_mode))
        fail("fstat");
    if (lseek(fd, 11, SEEK_SET) != 11 || read(fd, back, 3) != 3 || memcmp(back, "cst", 3) != 0)
        fail("lseek");
    close(fd);
    if (unlink("/m9/cstd.txt") != 0)
        fail("unlink");
    if (open("/m9/cstd.txt", O_RDONLY) != -1 || errno != ENOENT)
        fail("open after unlink");
    printf("cstdhello: files: wrote, read back, sought and unlinked /m9/cstd.txt (%ld bytes); then %s\n",
           (long)st.st_size, strerror(errno));
}

/* sigsetjmp with savemask: the mask at sigsetjmp comes back at siglongjmp. Linux's kernel set is one
   64-bit word (SIGUSR1 = 10 is bit 9); how is SIG_BLOCK 0 or SIG_SETMASK 2. cstd's <signal.h> has no
   sigset_t yet, so the prototype is spelled with a word. */
int sigprocmask(int how, const void *set, void *old);
static sigjmp_buf env;

static void check_sigsetjmp(void) {
    unsigned long usr1 = 1UL << 9, none = 0, now = 0;
    volatile int jumped = 0;
    if (sigprocmask(0, &usr1, 0) != 0)
        fail("sigprocmask");
    if (sigsetjmp(env, 1) == 0) {
        sigprocmask(2, &none, 0);
        jumped = 1;
        siglongjmp(env, 5);
    }
    sigprocmask(0, 0, &now);
    if (!jumped || (now & usr1) == 0)
        fail("siglongjmp did not restore the mask");
    sigprocmask(2, &none, 0);
    printf("cstdhello: sigsetjmp saved a blocked SIGUSR1 and siglongjmp put it back\n");
}

int main(int argc, char **argv) {
    if (atexit(handler_a) != 0 || atexit(handler_b) != 0 || atexit(handler_c) != 0)
        fail("atexit");
    check_printf();
    check_args(argc, argv);
    check_malloc();
    check_files();
    check_sigsetjmp();
    if (getpid() <= 1 || getppid() <= 0 || getppid() == getpid())
        fail("getpid");
    printf("cstdhello: getpid and getppid answered a child of the shell\n");
    return 0;
}
