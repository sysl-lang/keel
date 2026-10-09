# keel — roadmap

keel is a small Unix-like operating system for aarch64 (QEMU `virt`, HVF), written in sysl. Its
purpose is to force refinements to sysl: every gap it finds is recorded in `FINDINGS.md` and fixed in
the language rather than worked around here.

## The target (decided 2026-10-08)

- **A small Unix-like system** — not fully POSIX, but processes, files, a shell, and ordinary programs.
- **Multi-core.**
- **Kernel threads** — preemptive, blocking. Whether sysl's `async`/`Task` is used anywhere is
  decided later, by what the drivers turn up; kernel threads come first and do not depend on it.
- **Simple user programs can be written in sysl** — a `cp` command is the yardstick: a sysl program,
  built by `sysl build` for keel, using the standard library's files, running as a user process.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| 0 | Boot, UART, power off | `keel: hello`, QEMU exits ✅ |
| 1 | Exception vectors, GICv3, timer interrupt | `keel: trap brk`, `keel: tick 3` ✅ |
| 2 | MMU: page tables, kernel mapped high, device memory mapped as such | the kernel runs with the MMU on; `Atomic` works ✅ |
| 3 | Physical page allocator, kernel heap | `&T`, `Buf`, `string` work in the kernel ✅ |
| 4 | Kernel threads, preemptive scheduler, locks | two threads interleave on the timer; a mutex and a wait queue ✅ |
| 5 | Multi-core: secondary cores via PSCI `CPU_ON`, per-core data, SMP scheduler | threads run on all cores ✅ |
| 6 | User mode: EL0, system calls, per-process address spaces, ELF loading | a user program prints through a system call and exits ✅ |
| 7 | virtio-blk driver | sectors read and written from a disk image |
| 8 | A simple filesystem; `open`/`read`/`write`/`close`, directories | files survive a reboot |
| 9 | **sysl targets keel**: a target whose standard library reaches keel's system calls | `cp` written in sysl with `sysl.fs` copies a file |
| 10 | Processes and a shell: `fork`/`exec`/`wait` (or `spawn`), a serial shell, `ls`/`cat`/`cp` | a session at the shell |
