#!/bin/zsh
# keel's boot test: build the kernel, boot it under QEMU, and pass only if it says `keel: hello` and
# QEMU exits by itself (the kernel powered the machine off through PSCI).
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

elf=$out/keel.elf
log=$out/boot.log

$compiler build --target aarch64-freestanding ${opt:+$opt} -o $elf $root/kernel

if [[ $accel == hvf ]]; then
    machine=(-accel hvf -cpu host)
else
    machine=(-cpu cortex-a72)
fi

qemu-system-aarch64 -M virt $machine -m 256M -nographic -kernel $elf > $log 2>&1 < /dev/null &
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
if ! grep -q "keel: hello" $log; then
    print -r -- "FAIL: QEMU exited (status $qemu_exit) without printing 'keel: hello'"
    return 1
fi
if (( qemu_exit != 0 )); then
    print -r -- "FAIL: QEMU exited with status $qemu_exit"
    return 1
fi
print -r -- "PASS: 'keel: hello', and QEMU exited by itself (status 0) after $((waited / 10)).$((waited % 10))s"
