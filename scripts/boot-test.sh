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
#
# QEMU installs its own SIGALRM handler, so an alarm cannot bound it; the watchdog below kills it by
# PID instead.

emulate -L zsh
setopt err_return no_unset pipe_fail

root=${0:A:h:h}
out=$root/scratch
mkdir -p $out

limit=${KEEL_TIMEOUT:-15}
opt=${KEEL_OPT:-}
accel=${KEEL_ACCEL:-hvf}
compiler=${KEEL_SYSL:-sysl}

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
    "keel: virtio-blk found, capacity 8192 sectors"   # the 4 MiB image, found in a virtio-mmio slot
    "keel: disk read ok"    # sector 1 read back the bytes this script put there
    "keel: disk write ok"   # a pattern written to sector 2 read back the same
    "keel: disk smp ok"     # 4 threads x 100 reads at once, on >= 2 cores, every byte right
    "keel: fs mounted, 1024 blocks"   # the superblock mkfs wrote, checked against the disk's size
    "user: hello from EL0"  # a sysl program read from /bin, at EL0 in its own address space, printing through `write`
    "keel: process 1 exited 0"   # ...and ending with `exit(0)`
    "keel: process 2 killed: permission fault"   # a program reading kernel memory is killed, alone
    "keel: preempt user ok" # one spinner per core never yields; a worker still ran, and each was preempted
    "keel: bad pointer rejected"   # write() with four bad buffers answered EFAULT, and the kernel lives
    "user: echo one two"    # a program started with arguments got them, through sysl's main(args)
    "keel: argv ok"         # ...and 600 of them, more than fit at the top of the stack, were refused
    "keel: processes reclaimed"    # every frame, table and ASID the processes had came back
    "keel: fs smp ok"       # one process per core wrote, read back, removed a file of its own, synced
    "user: hello.txt says hello from the disk"   # a sysl program read a file mkfs put on the disk
    "user: write ok"        # ...wrote /out.txt (12 blocks, the indirect one too), reopened it, read it back
    "user: dirs ok"         # mkdir, getdents, rename (into and out of directories), unlink, rmdir, errors
)
after_user_line=(
    "keel: cp ok"           # cp, written with sysl.fs, copied /hello.txt to /m9, and cmp found the copy the same
    "keel: cp big ok"       # ...and the 200 KiB /m9/big.bin, through the indirect block (checked on the host too)
    "cp: /missing.txt: no such file or directory"   # cp of a missing file says sysl.fs's error...
    "keel: cp missing ok"   # ...and exits 1 (cmp of it exits 2)
    $'\nbin\ndir\nhello.txt\nm9\nout.txt\n'   # ls /, sorted, one name to a line
    "keel: ls ok"
    "inside a directory"    # cat /dir/inner.txt
    "keel: cat ok"
    "user: sysl.fs ok"      # fscheck: cwd, truncate, access, errors, unsupported calls, through sysl.fs
)

# Milestone 10: then init starts the shell, and the script is at the console. Each boot's session is
# piped into QEMU's serial input and its transcript checked, in order, between the lines above and the
# shutdown below. The console echoes a line as the shell reads it, so the transcript is the prompt,
# the command and its output, one after another.
session1=(
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
    'exit'
)
transcript1=(
    $'$ echo hello\nhello\n'                    # a program from /bin with its argv
    $'$ pwd\n/\n'                               # the built-in pwd, from the root
    $'$ mkdir /t\n$ cd /t\n$ pwd\n/t\n'         # mkdir, and cd — the shell's own directory
    $'$ echo data > f\n$ ls\nf\n'               # > made f in /t, and ls in the child's inherited cwd
    $'$ cat f\ndata\n'                          # ...holding what echo wrote to its redirected stdout
    $'$ cp f g\n$ rm f\n$ ls\ng\n'              # cp, rm: only g is left
    $'$ mv g h\n$ ls\nh\n'                      # mv
    $'$ mv h /m9/kept\n$ cd ..\n$ rmdir t\n$ ls -l /\nd '   # / is as the files program expects it next boot
    $'\nd       96 dir\n-       19 hello.txt\nd      192 m9\n'   # ls -l: kind and size, from sysl.fs.metadata
    $'$ mkdir /t\n$ mkdir /t/a\n$ echo one > /t/a/x\n$ cp -r /t /u\n$ ls -l /u/a\n-        4 x\n'   # copy_dir_all
    $'$ rm -r /t\n$ rm -r /u\n$ ls /\nbin\ndir\nhello.txt\nm9\nout.txt\n$ ppid\npid '   # remove_dir_all: both trees gone
    $' ppid 2\n'                                # ppid's parent is the shell, pid 2 (init is 1)
    $'$ orphan\n$ false\nsh: false: exit status 1\n'   # a failing command's status is said
    $'$ nosuch\nsh: nosuch: not found\n'        # ...and a missing one's
    $'$ exit\n'
    "init: the shell exited 0; orphans reaped: 1"   # orphan's child was handed to init and reaped
    "keel: init exited 0"
    "keel: every process reaped"
)
# The second boot finds what the first left and removes it, and edits a line: four characters typed,
# two rubbed out with DEL (each echoed as backspace, space, backspace), two more typed.
session2=(
    'cat /m9/kept'
    'rm /m9/kept'
    'ls /m9'
    $'echo tyxz\x7f\x7fpo'
    'exit'
)
transcript2=(
    $'$ cat /m9/kept\ndata\n'                   # the file the first boot's shell wrote, read back
    $'$ rm /m9/kept\n$ ls /m9\nbig.bin\nbig.copy\ncopy.txt\n'
    $'$ echo tyxz\b \b\b \bpo\ntypo\n'          # the line the shell got is the edited one
    $'$ exit\n'
    "init: the shell exited 0; orphans reaped: 0"
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
not_expected=("keel: bounds failure was not fatal" "user: FAILED" "init: sh ended by a fault"
    "keel: processes left unreaped" "keel: processes still running before init")

user_programs=(hello fault spin worker badptr files fsload echo cp cmp cat ls fscheck
    init sh mkdir rm rmdir mv false ppid orphan)

# 256 MiB is 65,536 frames; the device tree (1 MiB), the image and the bitmap take a few hundred.
frames_min=65000
frames_max=65536

elf=$out/keel.elf
mkfs=$out/mkfs

# The disk: a 4 MiB keel filesystem (1024 blocks) that mkfs builds from a directory holding
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

rm -rf $fsroot
mkdir -p $fsroot/dir $fsroot/bin
for prog in $user_programs; do
    cp $out/user/$prog.elf $fsroot/bin/$prog
done
print -n "hello from the disk" > $fsroot/hello.txt
print "inside a directory" > $fsroot/dir/inner.txt
mkdir -p $fsroot/m9
perl -e 'srand(9); print map { chr(int(rand(256))) } 1..204800' > $fsroot/m9/big.bin
$mkfs build $disk 1024 $fsroot
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

# One boot on the disk as it stands, passing only if every line of `before_user_line`, then $2, then
# `after_user_line`, then the array named $4, then `shutdown` is printed in order and QEMU powers off
# by itself. The array named $3 is typed at the console, a line each. $1 names the boot in the log.
boot() {
    local name=$1 user_line=$2
    local log=$out/boot$name.log input=$out/session$name.txt
    local expected=($before_user_line "$user_line" $after_user_line "${(@P)4}" $shutdown)

    print -rl -- "${(@P)3}" > $input
    qemu-system-aarch64 -M virt,gic-version=3 $machine -smp ${KEEL_SMP:-4} -m 256M -nographic $drive -kernel $elf > $log 2>&1 < $input &
    local qpid=$!

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

    print -r -- "--- QEMU output, boot $name ($accel${opt:+, $opt}) ---"
    cat $log
    print -r -- "---"

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
