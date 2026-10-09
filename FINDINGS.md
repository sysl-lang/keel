# keel — findings against sysl

Places where sysl 0.1.0-alpha.4 (self-hosted) could not express what a kernel needs cleanly,
ranked by how much each blocks a kernel.

**Findings 1-5 are resolved in sysl 0.1.0-alpha.5.** keel was moved onto the new forms with a
prerelease compiler built from the integration branch `census-34` at `7a59452170`, and boots at the
default level, at -O0 and under TCG. Each finding below keeps its original text and ends with how it
was resolved.

## 1. No entry point that runs before a stack exists (no naked function, no module-level asm)

Milestone 0, `_start`. Written:

    @export("_start")
    @section(".text.boot")
    start() -> never
        asm
            [aarch64]
                "adrp x0, __stack_top"
                "add x0, x0, :lo12:__stack_top"
                "mov sp, x0"
                "b kmain"
            [x86_64, thumb, riscv64, riscv32, craft, wasm32] unavailable "aarch64 only"

No diagnostic, but:
- At -O0 the published `_start` is an entry with a frame (reference/ffi.md § "The published symbol
  is an entry, not a renamed definition"):
      <_start>: f81f0ffe  str x30, [sp, #-0x10]!
                97fffd24  bl  <keel.boot$start>
  It stores through SP before SP is set, and the guest hangs. At the default level it works only
  because the optimizer inlines the definition into the entry.
- @section places the definition (keel.boot$start at the image base), not the published `_start`,
  which landed in plain .text.

Spec: inline-assembly.md has no naked function and no module-level asm; attributes.md
§ "What is deliberately absent" says the attribute set is closed.

Workaround (no .S): an unexported root-module function `_start` with @section(".text.boot"). It is
emitted under its bare name, kept by @section, and an asm-only body gets no frame at -O0/-O1.
Observed, not promised by the spec.

Clean form: `@naked`, or module-level `asm` in a named section. The milestone-1 vector table
(2 KiB aligned, 16 x 128-byte slots) needs it and has no workaround short of a .S file.

**Decided (user, 2026-10-08): module-level `asm` placed in a named section.**

**Resolved in sysl 0.1.0-alpha.5: assembly at the top of a file** (inline-assembly.md § "Assembly at
the top of a file"). `_start` is now an `asm` block outside every function under
`@section(".text.boot")`, with `.globl _start` / `_start:` labels emitted as written. At both -O1 and
-O0 the image's first instructions are `adrp/add` (relaxed by lld to `nop; adr x0, __stack_top`),
`mov sp, x0`, `b kmain`: no frame, at the image base 0x4008_0000, which is also the ELF entry.
`@align(n)` on such a block covers the milestone-1 vector table.

## 2. The generated code uses FP/SIMD and may assume unaligned access; no way to opt out

Probe: `struct Big { words: [64]u64 }`; an @export fn doing `*dst = *src` on `*Big` compiles to 96
q-register accesses for aarch64-freestanding. A kernel wants -mgeneral-regs-only (so exception
entry and context switch don't save 32 x 128-bit registers, and code that runs before FPEN is set
doesn't trap) and +strict-align (with the MMU off all data is Device memory, and an unaligned access
faults). There is no manifest key, flag or attribute for either.

Workaround: _start sets CPACR_EL1.FPEN=0b11; every exception entry will have to save the SIMD
file.

**Decided (user, 2026-10-08): a target/manifest setting for both.**

**Resolved in sysl 0.1.0-alpha.5: `codegen { general_regs_only = true, strict_align = true }`**
(packages.md § "What the generated code may use of the machine"). keel's `package.hocon` sets both;
`_start` no longer writes `CPACR_EL1`, so the FP/SIMD unit stays off. The linked image has no
FP/SIMD register operand at -O1 or -O0 (no `q`/`v`/`d`/`s`/`h`/`b` register in `llvm-objdump -d`).

## 3. `sysl build` cannot link a freestanding ELF

`sysl build --target aarch64-freestanding -o keel.elf kernel`:

    ld: unknown options: -Bstatic -EL --gc-sections --start-group --end-group

With SYSL_EXTRA_CFLAGS="-fuse-ld=lld":

    ld.lld: error: cannot open crt0.o / libclang_rt.builtins.a; unable to find library -lc

It links only with SYSL_EXTRA_CFLAGS="-fuse-ld=lld -nostdlib -Wl,-T,<abs>/link.ld". That variable
goes into every compile and cache key, and the build still emits an unused `main`. The manifest
has no key for linker, linker script or entry.

Workaround: `sysl build-c` + `ld.lld -T kernel/link.ld --gc-sections` by hand.

**Decided (user, 2026-10-08): manifest keys for linker, linker script and entry.**

**Resolved in sysl 0.1.0-alpha.5: `targets { aarch64-freestanding { linker_script, entry } }`**
(packages.md § "Linking a freestanding image"). The whole build is now
`sysl build --target aarch64-freestanding -o scratch/keel.elf kernel`: linked `-nostdlib` through
`ld.lld`, no environment flags, and no `main` is emitted (keel has no top-level statements). The
compiler also defines an absolute `__sysl_entry` at the entry, by design.

## 4. build-c advises `link this against: m` on a freestanding target (cosmetic)

There is no libm and nothing uses one; the link succeeds without it.

**Resolved in sysl 0.1.0-alpha.5**: `sysl build` of the image prints no `link this against` line.

## 5. Every build-c archive carries sysl.unicode.utf8proc.o (minor)

Unreferenced, but `--whole-archive` pulls in ~330 KiB of .rodata. Workaround: no --whole-archive;
ENTRY(_start) + KEEP(*(.text.boot)).

**Resolved in sysl 0.1.0-alpha.5**: `llvm-nm` of the linked image has no utf8proc or unicode symbol;
at -O1 the image is 0x4e4 bytes of .text and 0x14e of .rodata.

## What worked

- volatile u32 struct over ptr_cast(0x0900_0000): exact ldr/str, the poll loop is not hoisted.
- `extern __bss_start: u8` / `&__bss_start`; .bss is zeroed in sysl, no memset emitted.
- `asm "hvc #0"` with `in id : "x0"`: HVF answers PSCI SYSTEM_OFF and QEMU exits 0.
- @export("putchar") in keel.pl011 is picked up through the standard library's extern seam, so print works.
- A bounds trap calls the weak `sysl_bounds_fail` (a ret stub), then brk #1; a kernel can supply its own.
- No memcpy/memset calls in any freestanding object so far.
