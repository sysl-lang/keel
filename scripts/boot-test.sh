#!/bin/zsh
# keel's boot test: build the kernel, boot it under QEMU, and pass only if it prints the `expected`
# lines below in order and QEMU exits by itself (the kernel powered the machine off through PSCI).
# It boots twice on one disk: the filesystem the first boot writes is what the second one reads.
#
# The machine is `virt` with a GICv3: HVF refuses a GICv2 ("HVF does not support GICv2 emulation"),
# and asking for 3 explicitly gives TCG the same controller.
#
#   scripts/boot-test.sh            build at sysl's default level, run under HVF
#   KEEL_OPT=-O0 scripts/boot-test.sh
#   KEEL_ACCEL=tcg scripts/boot-test.sh   run under TCG (-cpu cortex-a72) instead of HVF
#   KEEL_SYSL=<path>/bin/sysl scripts/boot-test.sh   build with that compiler instead of `sysl`
#   KEEL_CSTD=<checkout> scripts/boot-test.sh   link /bin/cstdhello against that cstd (default ~/dev/sysl-lang/cstd)
#
# QEMU installs its own SIGALRM handler, so an alarm cannot bound it; the watchdog below kills it by
# PID instead.

emulate -L zsh
setopt err_return no_unset pipe_fail

root=${0:A:h:h}
out=$root/scratch
mkdir -p $out

limit=${KEEL_TIMEOUT:-25}
opt=${KEEL_OPT:-}
accel=${KEEL_ACCEL:-hvf}
compiler=${KEEL_SYSL:-sysl}
cstd=${KEEL_CSTD:-$HOME/dev/sysl-lang/cstd}

# Every boot prints these, in order. `user_line` is where each boot's own line goes: what the `files`
# program says about the previous boot's /out.txt.
before_user_line=(
    "keel: hello"           # kmain runs
    "keel: mmu on, kmain at 0xffff"   # ...in the high half, with the MMU on
    "keel: uart is Device-nGnRE"      # the MMU's own translation of the UART's address says so
    "keel: trap brk"        # a synchronous exception reaches the trap handler and returns
    "keel: tick 3"          # three timer interrupts arrived through the GIC, counted by an Atomic
    "keel: trap translation fault level 0"   # the low half is unmapped once the identity map is gone
    "keel: recovered from the fault"         # ...and the handler stepped over the faulting load
    "keel: frames "         # the frame allocator took RAM's size from the device tree (checked below)
    "keel: buf ok"          # a Buf[u64] grew to 10,000 elements on the kernel heap and summed right
    "keel: hello from the heap"              # a string built by interpolation
    "keel: closure ok"      # a closure capturing a boxed value
    "keel: aligned boxes ok"  # @align(64) and @align(4096) boxes on their boundaries
    "keel: heap reclaimed"  # every frame came back once those were dropped, and no block is live
    "keel: interleaved ok"  # two spinning threads took turns: only the timer can have switched them
    "keel: mutex ok"        # 4 threads x 10,000 additions under a blocking Mutex[u64] total 40,000
    "keel: waitqueue ok"    # a producer and a consumer slept on two wait queues and passed 200 in order
    "keel: stack guard faults"  # the page below a thread's stack is unmapped
    "keel: threads reclaimed"   # every joined thread's stack frames and heap blocks came back
    "keel: cores 4"         # PSCI CPU_ON started the three secondaries, and each checked in
    "keel: smp ran on "     # threads ran on more than one core (the count is checked below)
    "keel: smp mutex ok"    # 4 threads x 100,000 additions under the Mutex, on >= 2 cores at once
    "keel: smp heap ok"     # 4 threads x 10,000 Bufs on the heap at once; every block and frame back
    "keel: virtio-blk found, capacity 32768 sectors"   # the 16 MiB image, found in a virtio-mmio slot
    "keel: disk read ok"    # sector 1 read back the bytes this script put there
    "keel: disk write ok"   # a pattern written to sector 2 read back the same
    "keel: disk smp ok"     # 4 threads x 100 reads at once, on >= 2 cores, every byte right
    "keel: fs mounted, 4096 blocks"   # the superblock mkfs wrote, checked against the disk's size
    "user: hello from EL0"  # a sysl program read from /bin, at EL0 in its own address space, printing through `write`
    "keel: process 1 exited 0"   # ...and ending with `exit(0)`
    "keel: process 2 killed: permission fault"   # a program reading kernel memory is killed, alone
    "keel: preempt user ok" # one spinner per core never yields; a worker still ran, and each was preempted
    "keel: bad pointer rejected"   # write() with four bad buffers answered EFAULT, and the kernel lives
    "user: echo one two"    # a program started with arguments got them, through sysl's main(args)
    "keel: argv ok"         # ...and 600 of them, more than fit at the top of the stack, were refused
    "keel: processes reclaimed"    # every frame, table and ASID the processes had came back
    "user: fork ok"         # fork answered 0 and the child's pid; the child's writes stayed its own (milestone 15a)
    "user: cow ok"          # the parent's writes after the fork stayed its own; both sides copied every page
    "user: fork loop ok"    # 64 forks, each child writing a page and exiting with a status the parent waited for
    "user: exec echo ok"    # a forked child execve'd /bin/echo with argv and an environment
    "user: exec ok"         # /bin/env through a pipe saw the environment; /bin/false's 1; a failed execve's ENOENT
    "keel: forktest exited 0"
    "keel: fork frames reclaimed"  # every frame the forked children shared or copied came back, and every ASID
    "keel: fs smp ok"       # one process per core wrote, read back, removed a file of its own, synced
    "user: hello.txt says hello from the disk"   # a sysl program read a file mkfs put on the disk
    "user: write ok"        # ...wrote /out.txt (12 blocks, the indirect one too), reopened it, read it back
    "user: dirs ok"         # mkdir, getdents, rename (into and out of directories), unlink, rmdir, errors
    "user: pipes ok"        # pipe, dup2 to 9, dup, the end of input, EBADF, EPIPE (milestone 13)
)
after_user_line=(
    "keel: cp ok"           # cp, written with sysl.fs, copied /hello.txt to /m9, and cmp found the copy the same
    "keel: cp big ok"       # ...and the 200 KiB /m9/big.bin, through the indirect block (checked on the host too)
    "cp: /missing.txt: no such file or directory"   # cp of a missing file says sysl.fs's error...
    "keel: cp missing ok"   # ...and exits 1 (cmp of it exits 2)
    $'\nbin\ndir\netc\nhello.txt\nhome\nm9\nout.txt\n'   # ls /, sorted, one name to a line
    "keel: ls ok"
    "inside a directory"    # cat /dir/inner.txt
    "keel: cat ok"
    "user: sysl.fs ok"      # fscheck: cwd, truncate, access, errors, unsupported calls, through sysl.fs
    "keel: virtio-net found, mac 52:54:00:12:34:56"   # QEMU's default address, from the card's configuration
    "keel: ping gateway ok" # ARP found slirp's gateway, and it answered an ICMP echo
    "keel: udp echo served "   # port 7 in the kernel echoed the host client's datagrams until its "bye"
    "udpecho: listening on port 8007"   # a sysl program at EL0 bound a socket...
    "udpecho: bye after "   # ...and echoed the second client's datagrams through recvfrom/sendto
    "keel: udpecho exited 0"
    "keel: network down, sockets closed"   # the stack's thread joined; the program's socket closed at its exit
)

# Milestone 10: then init starts the shell, and the script is at the console. Each boot's session is
# piped into QEMU's serial input and its transcript checked, in order, between the lines above and the
# shutdown below. The console echoes a line as the shell reads it, so the transcript is the prompt,
# the command and its output, one after another.
#
# Milestone 12: init runs login, not the shell, and runs it again after every session; the end of the
# input at its prompt (Ctrl-D, the last line of each session) ends init. Boot 1 logs in as root for
# milestone 10's steps, then as alice: a wrong password, `id`, a root-only file refused, `login` refused
# to a user who is not root, `su` with a wrong and then the right password, the file read as root,
# then chmod and chown back as alice. The prompt is `# ` for root and `$ ` for anyone else.
session1=(
    'root'
    'keelroot'
    'echo hello'
    'pwd'
    'mkdir /t'
    'cd /t'
    'pwd'
    'echo data > f'
    'ls'
    'cat f'
    'cp f g'
    'rm f'
    'ls'
    'mv g h'
    'ls'
    'mv h /m9/kept'
    'cd ..'
    'rmdir t'
    'ls -l /'
    'mkdir /t'
    'mkdir /t/a'
    'echo one > /t/a/x'
    'cp -r /t /u'
    'ls -l /u/a'
    'rm -r /t'
    'rm -r /u'
    'ls /'
    'ppid'
    'orphan'
    'false'
    'nosuch'
    'whoami'
    'exit'
    'alice'
    'nope'
    'alice'
    'wonderland'
    'id'
    'pwd'
    'cat /etc/secret'
    'login'
    'root'
    'keelroot'
    'su'
    'nope'
    'su'
    'keelroot'
    'id'
    'cat /etc/secret'
    'exit'
    'echo note > note'
    'chmod 600 note'
    'ls -l'
    'chown root note'
    'su'
    'keelroot'
    'chown root:root /home/alice/note'
    'exit'
    'ls -l'
    'cat note'
    'whoami'
    'kill 1'
    'exit'
    $'\x04'
)
transcript1=(
    $'login: root\npassword: \n'        # init ran login; root's password opens root's account, read
                                                # with echo off (milestone 17): no password is ever on screen
    $'# echo hello\nhello\n'                    # a program from /bin with its argv, in root's shell
    $'# pwd\n/\n'                               # the built-in pwd, from root's home, the root
    $'# mkdir /t\n# cd /t\n# pwd\n/t\n'         # mkdir, and cd — the shell's own directory
    $'# echo data > f\n# ls\nf\n'               # > made f in /t, and ls in the child's inherited cwd
    $'# cat f\ndata\n'                          # ...holding what echo wrote to its redirected stdout
    $'# cp f g\n# rm f\n# ls\ng\n'              # cp, rm: only g is left
    $'# mv g h\n# ls\nh\n'                      # mv
    $'# mv h /m9/kept\n# cd ..\n# rmdir t\n# ls -l /\ndrwxr-xr-x root   '   # / is as the files program expects it next boot
    $'\ndrwxr-xr-x root         96 dir\ndrwxr-xr-x root        160 etc\n-rw-r--r-- root         19 hello.txt\n'
    $'drwxr-xr-x root         96 home\ndrwxr-xr-x root        192 m9\n'   # ls -l: mode, owner and size, from sysl.fs.metadata
    $'# mkdir /t\n# mkdir /t/a\n# echo one > /t/a/x\n# cp -r /t /u\n# ls -l /u/a\n-rw-r--r-- root          4 x\n'   # copy_dir_all
    $'# rm -r /t\n# rm -r /u\n# ls /\nbin\ndir\netc\nhello.txt\nhome\nm9\nout.txt\n# ppid\npid '   # remove_dir_all: both trees gone
    $' ppid 3\n'                                # ppid's parent is the shell, pid 3 (init is 1, login 2)
    $'# orphan\n# false\nsh: false: exit status 1\n'   # a failing command's status is said
    $'# nosuch\nsh: nosuch: not found\n'        # ...and a missing one's
    $'# whoami\nroot\n# exit\n'
    $'login: alice\npassword: \nlogin incorrect\n'   # init ran login again; a wrong password is refused
    $'login: alice\npassword: \n'
    $'$ id\nuid=1000(alice) gid=100(users)\n'   # alice's ids, named from /etc/passwd and /etc/group
    $'$ pwd\n/home/alice\n'                     # login went to her home directory
    $'$ cat /etc/secret\ncat: /etc/secret: permission denied\nsh: cat: exit status 1\n'   # mode 600, root's
    $'$ login\nlogin: root\npassword: \nlogin: cannot change user: operation not permitted\n'   # setgid/setuid: root only
    $'sh: login: exit status 1\n'
    $'$ su\npassword: \nsu: incorrect password\nsh: su: exit status 1\n'
    $'$ su\npassword: \n# id\nuid=0(root) gid=0(root)\n'   # su is set-user-ID root
    $'# cat /etc/secret\nfor root\'s eyes only\n# exit\n'   # root reads it; exit is back to alice
    $'$ echo note > note\n$ chmod 600 note\n$ ls -l\n-rw------- alice         5 note\n'   # chmod, by the owner
    $'$ chown root note\nchown: note: operation not permitted\nsh: chown: exit status 1\n'   # chown: root only
    $'$ su\npassword: \n# chown root:root /home/alice/note\n# exit\n'
    $'$ ls -l\n-rw------- root          5 note\n'   # it is root's now...
    $'$ cat note\ncat: note: permission denied\nsh: cat: exit status 1\n'   # ...so alice cannot read it
    $'$ whoami\nalice\n'
    $'$ kill 1\nkill: 1: operation not permitted\nsh: kill: exit status 1\n'   # milestone 15b: alice may not signal init, root's
    $'$ exit\nlogin: \n'                        # the end of the input at the login prompt ends init
    "init: the console closed after 2 sessions; orphans reaped: 1"   # orphan's child was handed to init and reaped
    "keel: init exited 0"
    "keel: every process reaped"
)
# The second boot finds what the first left and removes it, and edits a line: four characters typed,
# two rubbed out with DEL (each echoed as backspace, space, backspace), two more typed.
session2=(
    'root'
    'keelroot'
    'cat /m9/kept'
    'rm /m9/kept'
    'ls /m9'
    $'echo tyxz\x7f\x7fpo'
    'ls -l / | grep bin | wc -l'
    'ls / | grep o | head -n 2'
    'wc -l < /etc/group'
    'cat /m9/big.bin | wc -c'
    'cat /m9/big.bin | head -n 0'
    'echo one >> /m9/log'
    'echo two >> /m9/log'
    'cat /m9/log'
    'X=hello'
    'echo $X world'
    'false'
    'echo $?'
    'true'
    'echo $?'
    'echo $HOME $USER'
    'export GREETING=hi'
    'env | grep GREETING'
    'env > /m9/env.txt'
    'grep GREETING /m9/env.txt'
    'env | grep X | wc -l'
    'rm /m9/log'
    'rm /m9/env.txt'
    'fault'
    'echo $?'
    'spin x'
    '@wait spin: spinning'
    '@intr'
    'sigtest'
    '@wait sigtest: waiting for Ctrl-C'
    '@intr'
    'stdsig'
    'true | false &'
    'sleep 1'
    'sleep 100 &'
    'jobs'
    'ps'
    'kill %1'
    'sleep 100 &'
    'fg %1'
    $'@wait \nsleep 100\n'
    '@intr'
    'sleep 100 | sleep 100 &'
    'echo $!'
    'ps'
    'kill %1'
    'ls -l /bin > /m9/l'
    'cat /m9/l /m9/l /m9/l /m9/l /m9/l /m9/l | head -n 1 | wc -l'
    'rm /m9/l'
    'linuxhello'
    'linuxthreads'
    'linuxthreads exec'
    'linuxthreads fds'
    'threads'
    'linuxfp'
    'cstdhello one two'
    'echo $?'
    'linuxpoll'
    '@wait linuxpoll: termios round trip; raw mode, waiting for three bytes'
    '@raw abc'
    '@wait ISIG off, Ctrl-C is a byte'
    '@intr'
    '@wait termios restored'
    'busybox echo hello from busybox'
    'busybox uname -a'
    'busybox ls -l /'
    'busybox cat /etc/passwd | busybox wc -l'
    "busybox printf 'pear\\napple\\npear\\nfig\\napple\\n' > /m9/fruit"
    'busybox sort /m9/fruit'
    'busybox sort /m9/fruit | busybox uniq -c'
    "busybox sh -c 'for i in 1 2 3; do echo \$i; done'"
    "busybox sh -c 'false || echo fallback; echo \$?'"
    "busybox sh -c 'busybox ls / | busybox grep bin'"
    'busybox sleep 0.1'
    'busybox date -u +%Y'
    "busybox stat -c '%s %n' /hello.txt"
    'busybox du -s /dir'
    "busybox sh -c 'echo sleeping; exec busybox sleep 100'"
    $'@wait \nsleeping\n'
    '@intr'
    'busybox rm /m9/fruit'
    'busybox find /etc | busybox sort'
    'busybox md5sum /hello.txt'
    'busybox sha256sum /m9/big.bin'
    "busybox seq 3 | busybox tr '\\n' ' '"
    'busybox expr 6 * 7'
    'busybox sh'
    'echo interactive $((6 * 7))'
    'cd /etc; pwd'
    'exit 3'
    'echo $?'
    'exit'
    $'\x04'
)
transcript2=(
    $'login: root\npassword: \n'
    $'# cat /m9/kept\ndata\n'                   # the file the first boot's shell wrote, read back
    $'# rm /m9/kept\n# ls /m9\nbig.bin\nbig.copy\ncopy.txt\n'
    $'# echo tyxz\b \b\b \bpo\ntypo\n'          # the line the shell got is the edited one
    $'# ls -l / | grep bin | wc -l\n1\n'       # milestone 13: the done-when line, a pipeline of three
    $'# ls / | grep o | head -n 2\nhello.txt\nhome\n'   # another three stages; head stops at two
    $'# wc -l < /etc/group\n2\n'              # standard input from a file
    $'# cat /m9/big.bin | wc -c\n204800\n'    # 200 KiB through a 4 KiB pipe: the writer blocks and resumes
    $'# cat /m9/big.bin | head -n 0\n# echo one >> /m9/log\n'   # head ends at once: cat's write gets EPIPE, no hang
    $'# echo two >> /m9/log\n# cat /m9/log\none\ntwo\n'   # >> appends, twice
    $'# X=hello\n# echo $X world\nhello world\n'   # a shell variable, set and expanded
    $'# false\nsh: false: exit status 1\n# echo $?\n1\n'   # $? after a failure...
    $'# true\n# echo $?\n0\n'                 # ...and after a success
    $'# echo $HOME $USER\n/ root\n'           # login gave the shell its environment
    $'# export GREETING=hi\n# env | grep GREETING\nGREETING=hi\n'   # export reaches a child in a pipeline
    $'# env > /m9/env.txt\n# grep GREETING /m9/env.txt\nGREETING=hi\n'   # ...and one writing a file
    $'# env | grep X | wc -l\n0\n'           # X was never exported
    $'# rm /m9/log\n# rm /m9/env.txt\n'
    # Milestone 15b: signals and job control.
    $'# fault\nuser: reading the kernel\'s memory\nsh: fault: killed by signal 11\n# echo $?\n139\n'   # a fault is SIGSEGV
    $'# spin x\nspin: spinning\nsh: spin: killed by signal 2\n'   # Ctrl-C ends the foreground job, not the shell
    $'# sigtest\nsigtest: handler ok\nsigtest: mask ok\nsigtest: eintr ok\nsigtest: sigpipe ok\n'
    $'sigtest: sigchld ok\nsigtest: waiting for Ctrl-C\nsigtest: registers intact across Ctrl-C\n'   # a handler, and back
    $'# stdsig\nstdsig: watch ok\nstdsig: restart ok\nstdsig: ignore ok\nstdsig: group ok\n'   # sysl.signal and
                                                # sysl.process groups through keel.fsys's hooks; SA_RESTART
    $'stdsig: session ok\n'                     # set_process_group and new_session (setpgid, setsid 157)
    $'stdsig: zombie group ok\n'                # an ended, unreaped leader keeps its group joinable
    $'stdsig: time ok\n'                        # sysl.time's monotonic and now, through keel.fsys's seams
    $'# true | false &\n[1] '                   # a background pipeline that ends by itself: reaped by
    $'[1] exit status 1 true | false\n'         # try_wait (sysl_proc_poll), its last stage's status
    $'# sleep 100 &\n[1] '                      # a background job: its number and process group
    $'\n# jobs\n[1] running sleep 100\n'
    $'# ps\n  PID  PPID  PGID   UID CMD\n    1     0     1     0 init\n'   # the table, init first
    $'     0 sleep\n'                           # ...the background sleep in it
    $'     0 ps\n'
    $'# kill %1\n[1] killed by signal 15 sleep 100\n'   # SIGTERM to its group, reported at the next prompt
    $'# sleep 100 &\n[1] '
    $'\n# fg %1\nsleep 100\nsh: sleep 100: killed by signal 2\n'   # brought to the foreground, then Ctrl-C
    $'# sleep 100 | sleep 100 &\n[1] '         # a background pipeline: the LAST stage's pid, POSIX's
    $'\n# echo $!\n'                            # ...which $! answers too (the numbers: last_pid_check)
    $'\n# ps\n  PID  PPID  PGID   UID CMD\n'
    $'# kill %1\n[1] killed by signal 15 sleep 100 | sleep 100\n'   # kill %n still goes to the group
    $'# ls -l /bin > /m9/l\n# cat /m9/l /m9/l /m9/l /m9/l /m9/l /m9/l | head -n 1 | wc -l\n1\n'   # ~11 KiB of text
    $'# rm /m9/l\n'                             # through a 4 KiB pipe: head ends, cat's next write is SIGPIPE, no hang
    $'# linuxhello\nlinux: write, getpid and exit_group by Linux\'s numbers; pid '   # a static aarch64-linux
    $'\nsh: linuxhello: exit status 7\n'        # binary, built with no keel header: its raw calls are Linux's
    # Milestone 16: threads. A second raw-svc Linux program: clone with musl's flags, a futex lock, TLS, joins
    # by CLONE_CHILD_CLEARTID, a signal to the one thread not blocking it, and (from procs) >= 2 of its threads
    # running at once; then exit from the first thread and exit_group from another while a third spins.
    $'# linuxthreads\nlinuxthreads: 4 threads x 100000 under a futex lock: 400000; TLS and ids their own; joined\n'
    $'linuxthreads: futex EAGAIN, ETIMEDOUT, ENOSYS; the signal went to the thread not blocking it\n'
    $'linuxthreads: threads ran on different cores at once\n'
    $'linuxthreads: the first thread has exited; exit_group ends the rest\nsh: linuxthreads: exit status 6\n'
    $'# linuxthreads exec\nlinux: write, getpid and exit_group by Linux\'s numbers; pid '   # execve from a thread
    $'\nsh: linuxthreads: exit status 7\n'      # that is not the first: the others end first, the pid stays
    # The descriptor table under threads: four hammer pipe2/dup/openat/close at once, every descriptor claimed
    # by one thread; a read in flight keeps its pipe end through another thread's close; nothing left over.
    $'# linuxthreads fds\nlinuxthreads: 4 threads x 2000 rounds of pipe2, dup, openat and close at once: no descriptor shared, every pipe round-tripped\n'
    $'linuxthreads: a read in flight keeps its pipe end through a close; the table ends with 0-2 alone\n'
    # sysl.threads and sysl.time.sleep over keel.fsys's hooks (clone, futex, nanosleep): a contended Mutex
    # across cores, ids and thread-locals per thread (the first thread's laid by keel.user's start), a
    # Condvar handoff, a Channel's every value once, a sleep measured by the monotonic clock, every join.
    $'# threads\nthreads: 4 threads x 100000 under a Mutex: 400000, on different cores at once\n'
    $'threads: every thread its own id; thread-locals laid from the template for the first thread and each spawned one, and kept their own\n'
    $'threads: a Condvar handed the turn over 2000 times, every turn in order\n'
    $'threads: a Channel of 8 slots carried 3 producers x 5000 values, every one received once\n'
    $'threads: sleep(50 ms) took at least 50 ms by the monotonic clock\n'
    $'threads: 8 threads spawned, every one joined\n'
    # FP/SIMD at EL0: a raw-svc Linux program built without -mgeneral-regs-only. A new process's q registers,
    # FPSR and FPCR are zero; eight threads on four cores each carry their own through a long chain of SIMD
    # adds, across preemption and migration; a fork's child goes on from the parent's; a handler that
    # overwrites them all leaves the interrupted chain exact; execve (of itself, `linuxfp fresh`) zeroes them.
    $'# linuxfp\nlinuxfp: a new process starts with every q register, FPSR and FPCR zero\n'
    $'linuxfp: 8 threads x 1000000 rounds through q0-q31 on 4 cores: every sum exact, FPCR and FPSR their own\n'
    $'linuxfp: fork: parent and child each carried the q registers on; both sums exact\n'
    $'linuxfp: a handler that overwrote q0-q31, FPSR and FPCR left the interrupted sums exact\n'
    $'linuxfp: execve zeroed every q register, FPSR and FPCR\n'
    # A C program written against the C library alone: cstd's headers, crt0, printf (floats too), malloc
    # over brk, atexit, and open/read/write/lseek/fstat/unlink, all over Linux's calls; it exits 0.
    $'# cstdhello one two\ncstdhello: printf: -42 0x2a \'keel\' 3.142 2.50e-03 1e+21 !%\n'
    $'cstdhello: argv: 3 cstdhello one two; GREETING=hi among '
    $' environment strings\ncstdhello: malloc: 3000 blocks checked, 1000 grown by realloc, a '
    $'-byte block reused the freed run\ncstdhello: files: wrote, read back, sought and unlinked /m9/cstd.txt (16 bytes); then No such file or directory\n'
    $'cstdhello: getpid and getppid answered a child of the shell\ncstdhello: atexit: 3 handlers ran last-registered first\n# echo $?\n0\n'
    # Milestone 17: a third raw-svc Linux program. ppoll and pselect6 woken by a thread's write, a close and a
    # child's write, timing out, POLLNVAL/POLLERR/EBADF, their signal masks; the clocks; termios, and raw
    # mode driven from here: "abc" with no newline (no echo, so nothing between the two lines), then Ctrl-C
    # with ISIG off, read as a byte rather than sent as SIGINT.
    $'# linuxpoll\nlinuxpoll: ppoll woke on a thread\'s write and at its close (POLLHUP), timed out on time, POLLNVAL and POLLERR\n'
    $'linuxpoll: pselect6 woke on a child\'s write; a blocked SIGUSR1 came in through the call\'s mask alone: EINTR, handled, blocked again\n'
    $'linuxpoll: CLOCK_MONOTONIC advanced across nanosleep; clock_nanosleep TIMER_ABSTIME woke after its time; CLOCK_REALTIME is the RTC\'s date\n'
    $'linuxpoll: termios round trip; raw mode, waiting for three bytes\nlinuxpoll: raw read "abc" with no newline and no echo; ISIG off, Ctrl-C is a byte\n'
    $'linuxpoll: read Ctrl-C as 0x03; VTIME timed out an idle read; termios restored\n'
    # Milestone 18: Alpine's static musl busybox 1.37.0, unmodified, from keel's own shell. A static PIE,
    # loaded at 0x400000 and relocating itself from the auxiliary vector; uname answers keel's own names
    # (busybox adds "Linux", its compiled-in OS name); ls colours its names because isatty(1) holds on the
    # console; the owner is a number because keel's /etc/passwd has six fields and musl's getpwuid wants
    # Linux's seven, while /etc/group is Linux's format.
    $'# busybox echo hello from busybox\nhello from busybox\n'
    $'# busybox uname -a\nkeel keel 0.18 milestone 18 aarch64 Linux\n'
    $'# busybox ls -l /\ntotal 68\n'
    $'drwxr-xr-x    2 0        root          1536 Jan  1  1970 \e[1;34mbin\e[m\n'
    $'drwxr-xr-x    2 0        root            96 Jan  1  1970 \e[1;34mdir\e[m\n'
    $'drwxr-xr-x    2 0        root           160 Jan  1  1970 \e[1;34metc\e[m\n'
    $'-rw-r--r--    1 0        root            19 Jan  1  1970 \e[0;0mhello.txt\e[m\n'
    $'drwxr-xr-x    3 0        root            96 Jan  1  1970 \e[1;34mhome\e[m\n'
    $'drwxr-xr-x    2 0        root           224 Jan  1  1970 \e[1;34mm9\e[m\n'
    $'-rw-r--r--    1 0        root         45022 Jan  1  1970 \e[0;0mout.txt\e[m\n'
    # cat's sendfile (71) is ENOSYS on keel; busybox falls back to read and write, as it does on Linux
    # for a descriptor sendfile refuses.
    $'# busybox cat /etc/passwd | busybox wc -l\n2\n'
    $'# busybox printf \'pear\\napple\\npear\\nfig\\napple\\n\' > /m9/fruit\n'
    $'# busybox sort /m9/fruit\napple\napple\nfig\npear\npear\n'
    $'# busybox sort /m9/fruit | busybox uniq -c\n      2 apple\n      1 fig\n      2 pear\n'
    # busybox's own shell (ash): a loop, ||, $?, and a pipeline it forks and execs itself.
    $'# busybox sh -c \'for i in 1 2 3; do echo $i; done\'\n1\n2\n3\n'
    $'# busybox sh -c \'false || echo fallback; echo $?\'\nfallback\n0\n'
    $'# busybox sh -c \'busybox ls / | busybox grep bin\'\nbin\n'
    $'# busybox sleep 0.1\n# busybox date -u +%Y\n'"$(date -u +%Y)"$'\n'
    $'# busybox stat -c \'%s %n\' /hello.txt\n19 /hello.txt\n'
    $'# busybox du -s /dir\n8\t/dir\n'
    # Ctrl-C reaches the busybox sleep ash exec'd: it is in the console's foreground group.
    $'# busybox sh -c \'echo sleeping; exec busybox sleep 100\'\nsleeping\nsh: busybox: killed by signal 2\n'
    $'# busybox rm /m9/fruit\n'
    $'# busybox find /etc | busybox sort\n/etc\n/etc/group\n/etc/passwd\n/etc/secret\n'
    # The sums are the host's, over the same bytes the script puts on the disk below.
    $'# busybox md5sum /hello.txt\n'"$(print -n 'hello from the disk' | md5 -q)"$'  /hello.txt\n'
    $'# busybox sha256sum /m9/big.bin\n'"$(perl -e 'srand(9); print map { chr(int(rand(256))) } 1..204800' | shasum -a 256 | cut -d' ' -f1)"$'  /m9/big.bin\n'
    $'# busybox seq 3 | busybox tr \'\\n\' \' \'\n1 2 3 # busybox expr 6 * 7\n42\n'
    # busybox's shell, interactive: its line editor puts the console in raw mode and echoes for itself;
    # ~ is HOME (root's is /), and `exit 3` comes back to keel's shell as the job's status.
    $'# busybox sh\n~ # echo interactive $((6 * 7))\ninteractive 42\n~ # cd /etc; pwd\n/etc\n'
    $'/etc # exit 3\nsh: busybox: exit status 3\n# echo $?\n3\n'
    $'# exit\nlogin: \n'
    "init: the console closed after 1 sessions; orphans reaped: 0"
    "keel: init exited 0"
    "keel: every process reaped"
)
shutdown=(
    "keel: fs synced at shutdown"   # the dirty blocks went to the disk before the power did
    "keel: bounds check failed: index"   # the last step: a kernel bounds failure is reported...
    "keel: halted"                 # ...and stops the machine (checked below: nothing ran after it)
)

# Printed only if something went wrong: the access after a failed bounds check went ahead, a check in
# the `files` program failed, the shell was restarted, or a process was left behind.
not_expected=("keel: bounds failure was not fatal" "user: FAILED" "init: login ended by signal"
    "keel: processes left unreaped" "keel: processes still running before init" "linuxpoll: FAILED"
    "linuxfp: FAILED" "threads: FAILED" "cstdhello: FAILED"
    "keelroot" "wonderland" "nope")

user_programs=(hello fault spin worker badptr files fsload echo cp cmp cat ls fscheck
    init sh mkdir rm rmdir mv false ppid orphan udpecho login su id whoami chmod chown env wc grep head true forktest
    sigtest stdsig ps kill sleep threads)

# 256 MiB is 65,536 frames; the device tree (1 MiB), the image and the bitmap take a few hundred.
frames_min=65000
frames_max=65536

elf=$out/keel.elf
mkfs=$out/mkfs

# The disk: a 16 MiB keel filesystem (4096 blocks; the programs at -O0 and busybox's 273 overflow 2048) that mkfs builds from a directory holding
# /hello.txt and /dir/inner.txt. Its block 0 is a boot block the filesystem never uses, which is where
# milestone 7's test lives: sector 1 gets byte i = (7i + 3) mod 256 here, the kernel writes sector 2
# with byte i = (13i + 0x5a) mod 256, and after the first boot the script reads it back from the file.
# keel.selftest's disk.sysl has the same two formulas.
disk=$out/disk.img
fsroot=$out/fsroot
sector() { perl -e 'my ($m, $a) = @ARGV; print map { chr(($_ * $m + $a) & 0xff) } 0..511' $1 $2 }
sector 13 90 > $out/sector2.expected

# The host's mkfs, the user programs and the kernel. The programs are not in the kernel's image: they
# go into the filesystem's /bin, and the kernel reads each from there when it starts one.
$compiler build -o $mkfs $root/tools/mkfs
mkdir -p $out/user
for prog in $user_programs; do
    $compiler build --target aarch64-freestanding ${opt:+$opt} -o $out/user/$prog.elf $root/user/$prog
done
$compiler build --target aarch64-freestanding ${opt:+$opt} -o $elf $root/kernel

# A static aarch64 Linux program, built with a Linux target's toolchain and no keel header: it proves
# that keel's system-call numbers are Linux's. LLVM's clang and ld.lld from Homebrew unless told otherwise.
linux_cc=${KEEL_LINUX_CC:-/opt/homebrew/opt/llvm/bin/clang}
linux_ld=${KEEL_LINUX_LD:-/opt/homebrew/bin/ld.lld}
$linux_cc --target=aarch64-linux-gnu -ffreestanding -fno-builtin -nostdlib -static -fno-pic -O2 -mgeneral-regs-only \
    -c -o $out/linuxhello.o $root/linux/hello.c
$linux_ld -static -e _start --image-base=0x400000 -z max-page-size=4096 -o $out/linuxhello $out/linuxhello.o
$linux_cc --target=aarch64-linux-gnu -ffreestanding -fno-builtin -nostdlib -static -fno-pic -O2 -mgeneral-regs-only \
    -mno-outline-atomics -c -o $out/linuxthreads.o $root/linux/threads.c
$linux_ld -static -e _start --image-base=0x400000 -z max-page-size=4096 -o $out/linuxthreads $out/linuxthreads.o
$linux_cc --target=aarch64-linux-gnu -ffreestanding -fno-builtin -nostdlib -static -fno-pic -O2 -mgeneral-regs-only \
    -mno-outline-atomics -c -o $out/linuxpoll.o $root/linux/poll.c
$linux_ld -static -e _start --image-base=0x400000 -z max-page-size=4096 -o $out/linuxpoll $out/linuxpoll.o
# The one built WITHOUT -mgeneral-regs-only: it computes in the FP/SIMD registers on purpose.
$linux_cc --target=aarch64-linux-gnu -ffreestanding -fno-builtin -nostdlib -static -fno-pic -O2 \
    -mno-outline-atomics -c -o $out/linuxfp.o $root/linux/fp.c
$linux_ld -static -e _start --image-base=0x400000 -z max-page-size=4096 -o $out/linuxfp $out/linuxfp.o

# Milestone 18: a static aarch64 musl busybox, UNMODIFIED — Alpine's own `busybox-static` package (an
# apk is a gzip'd tar; the program is bin/busybox.static, a static PIE). It is fetched once into a
# directory of its own under scratch/, both the package and the program are checked against the
# sha256s below, and it is never run on the host. The test fails, saying so, if it is not there and
# cannot be fetched.
busybox_url=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/aarch64/busybox-static-1.37.0-r31.apk
busybox_apk_sha256=965777e06b94bf11981d5f4ecdfcd577879f4b0dda544294d1dd3f72b217bc75
busybox_sha256=999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22
busybox_dir=$out/busybox-1.37.0-r31
busybox=$busybox_dir/bin/busybox.static
if [[ ! -f $busybox ]]; then
    rm -rf $busybox_dir
    mkdir -p $busybox_dir
    if ! curl -sfL -o $busybox_dir/busybox-static.apk $busybox_url; then
        print -r -- "FAIL: busybox is not in $busybox_dir and could not be fetched from $busybox_url"
        return 1
    fi
    if [[ $(shasum -a 256 $busybox_dir/busybox-static.apk) != "$busybox_apk_sha256 "* ]]; then
        print -r -- "FAIL: $busybox_url is not the package this test was written against (sha256)"
        return 1
    fi
    tar -xzf $busybox_dir/busybox-static.apk -C $busybox_dir bin/busybox.static
fi
if [[ $(shasum -a 256 $busybox) != "$busybox_sha256 "* ]]; then
    print -r -- "FAIL: $busybox is not busybox 1.37.0-r31's bin/busybox.static (sha256)"
    return 1
fi

# A C program against a C library: cstd's Linux half is archived for aarch64 with the portable half
# inside it (its `_start` calls the program's `main`), and the program is compiled against cstd's
# headers alone, with clang's freestanding ones for stddef.h and stdarg.h. Unlike the raw-svc programs
# it is not -mgeneral-regs-only: printf takes doubles, so the program and cstd use the FP/SIMD
# registers, which keel has to enable at EL0 and save per thread.
$compiler build-c $cstd/linux --target aarch64-freestanding ${opt:+$opt} -o $out/libcstd.a
$linux_cc --target=aarch64-linux-gnu -ffreestanding -nostdinc -isystem "$($linux_cc -print-resource-dir)/include" \
    -I $cstd/include -static -fno-pic -O2 -c -o $out/cstdhello.o $root/linux/cstdhello.c
$linux_ld -static -e _start --image-base=0x400000 -z max-page-size=4096 -o $out/cstdhello $out/cstdhello.o $out/libcstd.a

rm -rf $fsroot
mkdir -p $fsroot/dir $fsroot/bin
cp $busybox $fsroot/bin/busybox
for prog in $user_programs; do
    cp $out/user/$prog.elf $fsroot/bin/$prog
done
cp $out/linuxhello $fsroot/bin/linuxhello
cp $out/linuxthreads $fsroot/bin/linuxthreads
cp $out/linuxpoll $fsroot/bin/linuxpoll
cp $out/linuxfp $fsroot/bin/linuxfp
cp $out/cstdhello $fsroot/bin/cstdhello
print -n "hello from the disk" > $fsroot/hello.txt
print "inside a directory" > $fsroot/dir/inner.txt
mkdir -p $fsroot/m9
perl -e 'srand(9); print map { chr(int(rand(256))) } 1..204800' > $fsroot/m9/big.bin
# Milestone 12: the users. root's password is `keelroot`, alice's `wonderland`; /etc/passwd holds their
# hashes (mkfs computes them with keel.passwd, the code login checks them with). Everything is root's
# except alice's home, /bin/su is set-user-ID root, and /etc/secret is root's alone.
mkdir -p $fsroot/etc $fsroot/home/alice
print -r -- "root:$($mkfs passwd root keelroot):0:0:/:/bin/sh" > $fsroot/etc/passwd
print -r -- "alice:$($mkfs passwd alice wonderland):1000:100:/home/alice:/bin/sh" >> $fsroot/etc/passwd
print -l "root:x:0:" "users:x:100:alice" > $fsroot/etc/group
print "for root's eyes only" > $fsroot/etc/secret
owners=$out/owners
print -l "/home/alice 1000 100 755" "/bin/su 0 0 4755" "/etc/secret 0 0 600" > $owners
$mkfs build $disk 4096 $fsroot $owners

# The same tree and owners go into a 32 MiB KFS image through mkfs.kfs (the kernel does not mount it yet):
# it has to be fsck-clean, and big files must read back byte for byte through kfsdump.
kfs_disk=$out/kfs-disk.img
$compiler build -o $out/mkfs.kfs $root/tools/mkfs-kfs
$compiler build -o $out/kfsdump $root/tools/kfsdump
$compiler build -o $out/fsck.kfs $root/tools/fsck-kfs
if ! $out/mkfs.kfs $kfs_disk 8192 -d $fsroot -o $owners; then
    echo "host: mkfs.kfs could not build the fsroot into 32 MiB"
    exit 1
fi
if ! $out/fsck.kfs -n $kfs_disk; then
    echo "host: the KFS image of the fsroot is not clean"
    exit 1
fi
for f in /m9/big.bin /bin/busybox /hello.txt /dir/inner.txt; do
    if ! $out/kfsdump get $kfs_disk $f $out/kfs-out; then
        echo "host: kfsdump get $f failed"
        exit 1
    fi
    if ! cmp $fsroot$f $out/kfs-out; then
        echo "host: $f differs after a trip through the KFS image"
        exit 1
    fi
done
echo "host: kfs fsroot ok"
sector 7 3 | perl -e 'open(my $f, "+<", $ARGV[0]) or die; seek($f, 512, 0); local $/; print $f <STDIN>' $disk

if [[ $accel == hvf ]]; then
    machine=(-accel hvf -cpu host)
else
    machine=(-cpu cortex-a72)
fi

# QEMU's virtio-mmio transport is the legacy (version 1) interface unless told otherwise; keel drives
# the modern one.
drive=(-drive file=$disk,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0
       -global virtio-mmio.force-legacy=false)

# The network: QEMU's user-mode stack (slirp), keel at 10.0.2.15, with two host UDP ports forwarded —
# $udp_port to keel's port 7 (the kernel's echo service) and the next one to 8007 (/bin/udpecho). A
# client on the host (udp-echo-client.pl) talks to each while keel runs.
udp_port=${KEEL_UDP_PORT:-15007}
user_udp_port=$((udp_port + 1))
nic=(-netdev user,id=n0,hostfwd=udp:127.0.0.1:$udp_port-:7,hostfwd=udp:127.0.0.1:$user_udp_port-:8007
     -device virtio-net-device,netdev=n0)

# Type the session named $1 at the console: QEMU's standard input is the FIFO $3, and $2 is its output
# so far. Each element is a line, sent with its newline, except three directives:
#   @wait <text>   send nothing more until <text> has been printed after what the last @wait matched,
#                  so a byte reaches the console only once the program it is meant for is running
#   @intr          Ctrl-C, alone: the console turns it into SIGINT for its foreground process group
#   @raw <text>    <text> with no newline, for a program reading the console in raw mode
# A @wait that is never satisfied gives up after the boot's time limit; the boot fails on its own then.
# The console's output has Linux's ONLCR (a newline goes out as CR LF); the CRs are dropped before any
# text is compared, here and after the boot.
feed() {
    local item want text rest before offset=0 waited fd
    zmodload zsh/mapfile
    exec {fd}> $3
    for item in "${(@P)1}"; do
        if [[ $item == '@wait '* ]]; then
            want=${item#@wait }
            waited=0
            while (( waited < limit * 10 )); do
                # The output as it stands, trailing newlines and all (`$(<file)` would drop them); an
                # empty file reads as unset.
                text=${${mapfile[$2]-}//$'\r'/}
                rest=${text:$offset}
                if [[ $rest == *$want* ]]; then
                    before=${rest%%$want*}
                    offset=$(( offset + ${#before} + ${#want} ))
                    break
                fi
                sleep 0.1
                waited=$((waited + 1))
            done
        elif [[ $item == '@intr' ]]; then
            print -rn -- $'\x03' >&$fd
        elif [[ $item == '@raw '* ]]; then
            print -rn -- "${item#@raw }" >&$fd
        else
            print -r -- "$item" >&$fd
        fi
    done
    exec {fd}>&-
}

# One boot on the disk as it stands, passing only if every line of `before_user_line`, then $2, then
# `after_user_line`, then the array named $4, then `shutdown` is printed in order and QEMU powers off
# by itself. The array named $3 is typed at the console, a line each. $1 names the boot in the log.
boot() {
    local name=$1 user_line=$2
    local log=$out/boot$name.log input=$out/session$name.txt
    local expected=($before_user_line "$user_line" $after_user_line "${(@P)4}" $shutdown)

    print -rl -- "${(@P)3}" > $input
    local fifo=$out/console$name.fifo
    rm -f $fifo
    mkfifo $fifo
    : > $log
    qemu-system-aarch64 -M virt,gic-version=3 $machine -smp ${KEEL_SMP:-4} -m 256M -nographic $drive $nic -kernel $elf > $log 2>&1 < $fifo &
    local qpid=$!
    feed $3 $log $fifo &
    local fpid=$!
    local udp_log=$out/udp$name.log
    perl $root/scripts/udp-echo-client.pl $udp_port $limit kernel > $udp_log.kernel 2>&1 &
    local kclient=$!
    perl $root/scripts/udp-echo-client.pl $user_udp_port $limit user > $udp_log.user 2>&1 &
    local uclient=$!

    waited=0
    while kill -0 $qpid 2> /dev/null && (( waited < limit * 10 )); do
        sleep 0.1
        waited=$((waited + 1))
    done

    local exited=yes
    if kill -0 $qpid 2> /dev/null; then
        exited=no
        kill -9 $qpid 2> /dev/null
    fi
    local qemu_exit=0
    wait $qpid 2> /dev/null || qemu_exit=$?
    # A client still running once keel is gone never got its echo; it is stopped by PID, and so is a
    # feeder still waiting for a line that never came.
    kill $kclient $uclient $fpid 2> /dev/null || true
    wait $fpid 2> /dev/null || true
    rm -f $fifo
    wait $kclient 2> /dev/null || true
    wait $uclient 2> /dev/null || true
    perl -pi -e 's/\r//g' $log

    print -r -- "--- QEMU output, boot $name ($accel${opt:+, $opt}) ---"
    cat $log
    print -r -- "---"
    cat $udp_log.kernel $udp_log.user

    if [[ $exited == no ]]; then
        print -r -- "FAIL: boot $name: QEMU did not exit within ${limit}s; the kernel did not power off"
        return 1
    fi
    local text=$(<$log) seen="" line rest
    for line in $expected; do
        rest=${text#*$line}
        if [[ $rest == $text ]]; then
            print -r -- "FAIL: boot $name: QEMU exited (status $qemu_exit); expected '$line'${seen:+ after$seen}, and it is not there"
            return 1
        fi
        text=$rest
        seen="$seen '$line'"
    done
    for line in $not_expected; do
        if [[ $(<$log) == *"$line"* ]]; then
            print -r -- "FAIL: boot $name: '$line' was printed"
            return 1
        fi
    done
    local frames=${${(M)${(f)"$(<$log)"}:#keel: frames *}#keel: frames }
    if [[ $frames != <-> ]] || (( frames < frames_min || frames > frames_max )); then
        print -r -- "FAIL: boot $name: 'keel: frames $frames' is not a free count between $frames_min and $frames_max"
        return 1
    fi
    local spread=${${(M)${(f)"$(<$log)"}:#keel: smp ran on *}#keel: smp ran on }
    spread=${spread% cores}
    if [[ $spread != <-> ]] || (( spread < 2 )); then
        print -r -- "FAIL: boot $name: 'keel: smp ran on $spread cores' — threads have to have run on at least 2 cores"
        return 1
    fi
    if (( qemu_exit != 0 )); then
        print -r -- "FAIL: boot $name: QEMU exited with status $qemu_exit"
        return 1
    fi
    local client
    for client in kernel user; do
        if [[ $(<$udp_log.$client) != *"host: udp echo ok ($client)"* ]]; then
            print -r -- "FAIL: boot $name: the host's UDP client for the $client echo got no echo"
            return 1
        fi
    done
}

# The image is well-formed after a boot: mkfs's check reads every directory, inode and bitmap bit.
fsck() {
    if ! $mkfs check $disk; then
        print -r -- "FAIL: after boot $1, the filesystem on $disk is damaged"
        return 1
    fi
}

boot 1 "user: first boot, no /out.txt yet" session1 transcript1
first=$waited

# The kernel's write reached the image file: sector 2, read on the host once QEMU has gone.
if [[ $(od -An -tx1 -v -j 1024 -N 512 $disk) != $(od -An -tx1 -v $out/sector2.expected) ]]; then
    print -r -- "FAIL: sector 2 of $disk does not hold what the kernel wrote:"
    od -An -tx1 -v -j 1024 -N 32 $disk
    return 1
fi
print -r -- "keel: disk persisted"
fsck 1

boot 2 "user: persisted across reboot" session2 transcript2
second=$waited
fsck 2

# `sleep 100 | sleep 100 &` printed its last stage's pid, as a POSIX shell does: `echo $!` printed the
# same number, and ps lists it as a sleep whose group is another number — the first stage's, which ps
# lists as a sleep leading that group.
last_pid_check() {
    perl -0777 -ne '
        /# sleep 100 \| sleep 100 &\n\[1\] (\d+)\n# echo \$!\n(\d+)\n# ps\n(.*?)\n# kill %1/s
            or die "the background pipeline, its \$! and its ps are not in the log\n";
        my ($printed, $bang, $ps) = ($1, $2, $3);
        $bang == $printed or die "\$! is $bang, but the shell printed [1] $printed\n";
        $ps =~ /^ *$printed +\d+ +(\d+) +\d+ sleep$/m or die "ps lists no sleep with pid $printed\n";
        my $group = $1;
        $group != $printed or die "pid $printed is its own group: the shell printed the first stage\n";
        $ps =~ /^ *$group +\d+ +$group +\d+ sleep$/m or die "ps lists no sleep leading group $group\n";
        print "keel: background pipeline [1] $printed = \$!, group $group\n";
    ' $out/boot2.log
}
if ! last_pid_check; then
    print -r -- "FAIL: boot 2: the background pipeline's number is not its last stage's pid"
    return 1
fi

# And the host reads what the second boot wrote, through the same format.
$mkfs get $disk /out.txt $out/out.txt
header=$(head -1 $out/out.txt)
if [[ $header != "keel was here, boot 2" ]]; then
    print -r -- "FAIL: /out.txt on the host begins '$header', not 'keel was here, boot 2'"
    return 1
fi
print -r -- "host: /out.txt says $header"

# The big file cp copied on keel, read back on the host through mkfs and compared byte for byte.
rm -f $out/big.copy
$mkfs get $disk /m9/big.copy $out/big.copy
if ! cmp $fsroot/m9/big.bin $out/big.copy; then
    print -r -- "FAIL: /m9/big.copy on the host is not /m9/big.bin"
    return 1
fi
print -r -- "host: cp verified"
print -r -- "PASS: two boots on one disk, every line in order each time, and QEMU exited by itself (status 0) after $((first / 10)).$((first % 10))s and $((second / 10)).$((second % 10))s"
