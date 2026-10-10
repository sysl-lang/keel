/* A static aarch64 Linux program that knows nothing of keel: no C library, no keel headers, only raw
   system calls by their numbers in Linux's asm-generic table. It writes a line naming its pid and ends
   with exit_group(7), so the boot test sees write (64), getpid (172) and exit_group (94) all arrive.
   scripts/boot-test.sh builds it for aarch64-linux with clang and ld.lld and puts it on the disk as
   /bin/linuxhello. */

typedef unsigned long u64;

#define SYS_write 64
#define SYS_exit_group 94
#define SYS_getpid 172

static long call1(long nr, long a0) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long call3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static const char said[] = "linux: write, getpid and exit_group by Linux's numbers; pid ";

void _start(void) {
    char line[96];
    u64 n = 0;
    for (; said[n] != 0; n++)
        line[n] = said[n];

    long pid = call1(SYS_getpid, 0);
    char digits[20];
    int k = 0;
    do {
        digits[k++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (k > 0)
        line[n++] = digits[--k];
    line[n++] = '\n';

    call3(SYS_write, 1, (long)line, (long)n);
    call1(SYS_exit_group, 7);
    for (;;)
        ;
}
