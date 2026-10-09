#!/bin/zsh
# keel's boot test: build the kernel, boot it under QEMU, and pass only if it prints the `expected`
# lines below in order and QEMU exits by itself (the kernel powered the machine off through PSCI).
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

expected=(
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
)

# 256 MiB is 65,536 frames; the device tree (1 MiB), the image and the bitmap take a few hundred.
frames_min=65000
frames_max=65536

elf=$out/keel.elf
log=$out/boot.log

$compiler build --target aarch64-freestanding ${opt:+$opt} -o $elf $root/kernel

if [[ $accel == hvf ]]; then
    machine=(-accel hvf -cpu host)
else
    machine=(-cpu cortex-a72)
fi

qemu-system-aarch64 -M virt,gic-version=3 $machine -m 256M -nographic -kernel $elf > $log 2>&1 < /dev/null &
qpid=$!

waited=0
while kill -0 $qpid 2> /dev/null && (( waited < limit * 10 )); do
    sleep 0.1
    waited=$((waited + 1))
done

exited=yes
if kill -0 $qpid 2> /dev/null; then
    exited=no
    kill -9 $qpid 2> /dev/null
fi
qemu_exit=0
wait $qpid 2> /dev/null || qemu_exit=$?

print -r -- "--- QEMU output ($accel${opt:+, $opt}) ---"
cat $log
print -r -- "---"

if [[ $exited == no ]]; then
    print -r -- "FAIL: QEMU did not exit within ${limit}s; the kernel did not power off"
    return 1
fi
text=$(<$log)
seen=""
for line in $expected; do
    rest=${text#*$line}
    if [[ $rest == $text ]]; then
        print -r -- "FAIL: QEMU exited (status $qemu_exit); expected '$line'${seen:+ after$seen}, and it is not there"
        return 1
    fi
    text=$rest
    seen="$seen '$line'"
done
frames=${${(M)${(f)"$(<$log)"}:#keel: frames *}#keel: frames }
if [[ $frames != <-> ]] || (( frames < frames_min || frames > frames_max )); then
    print -r -- "FAIL: 'keel: frames $frames' is not a free count between $frames_min and $frames_max"
    return 1
fi
if (( qemu_exit != 0 )); then
    print -r -- "FAIL: QEMU exited with status $qemu_exit"
    return 1
fi
print -r -- "PASS:$seen in order, and QEMU exited by itself (status 0) after $((waited / 10)).$((waited % 10))s"
