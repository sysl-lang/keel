# keel — findings against sysl

Places where sysl 0.1.0-alpha.4 (self-hosted) could not express what a kernel needs cleanly,
ranked by how much each blocks a kernel. Findings 1-5 are from milestone 0, 6-11 from milestone 1.

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

## Milestone 1: exceptions and a timer interrupt

Findings 6-11 are against the prerelease alpha.5 (`census-34` at `7a59452170`), found while writing
the vector table, the trap handler, the GICv3 driver and the generic timer. Ranked by how much each
blocks a kernel. None was fixed here; where keel needed a way round, it says which.

## 6. State shared with an interrupt handler: `volatile` is refused on module storage, and `Atomic` faults with the MMU off

Milestone 1, `keel.irq.ticks`: the IRQ handler counts timer ticks, kmain waits for three.

A plain module `var` is correct only by accident. With the loop body empty:

    var ticks: u64 = 0                      // keel.irq; the IRQ handler does ticks += 1
    while irq.ticks < 3 do ()               // kmain

compiles (-O1) to one load and a branch to itself:

    4008014c: ldr  x8, [x19, #0x480]
    40080150: cmp  x8, #0x2
    40080154: b.hi 0x400801a0
    40080158: b    0x40080158               // spins for ever; the handler's writes are never seen

That is right for sysl's model, where nothing else writes the variable. keel's real loop calls
`wait_for_interrupt()`, an `asm "wfi"`, and an asm block is assumed to clobber memory, so the load is
redone each time round. That is the only reason keel works.

Declaring the sharing is refused:

    var ticks: volatile u64 = 0

    error: 'volatile u64' is the type of *storage*, and this is a value — what a read of a volatile
    place hands back is an ordinary 'u64'. The qualifier goes where the storage is named: a struct
    field, an element, or the pointee of a '*T', as '*volatile u64'

But a module `var` is storage, and it is named right there.

The documented answer is `sysl.sync.Atomic` (api/sysl-sync.md: "a freestanding program that shares a
word with an interrupt needs them exactly as much as a hosted one that shares it with a thread").
On AArch64 with the MMU off it **faults**. `var ticks: Atomic[u64] = Atomic(0)`, `ticks.add(1,
Relaxed)` in the handler, and the first tick takes a data abort. keel's own trap handler reported it:

    keel: trap DataAbort esr=0x0000000096000035 elr=0x0000000040081dc8 far=0x0000000040082530

DFSC 0x35 is "unsupported exclusive or atomic access": `add` is an `ldxr`/`stxr` loop (no LSE), and
with the MMU off every access is Device memory, where exclusives are not supported. A uniprocessor
kernel before its MMU is on needs only a compiler barrier for this word, and sysl can say that only
through a struct.

Workaround that works (probed, not used): a field may be volatile, so wrap the word:

    struct Count
        n: volatile u64

    var ticks: Count = Count(0)             // the loop then reloads ticks.n every iteration

keel keeps the plain `var` and relies on the `wfi` block's memory clobber, with a comment saying so.

Milestone 2: with the MMU on and RAM mapped Normal, `Atomic` works, and `keel.irq.ticks` is now an
`Atomic[u64]`. The half about code that runs before the MMU (or with a device's memory) still stands.

Clean form: `volatile` accepted on a module `var` (it is storage), or an ordering-only access in
`sysl.sync` (a load/store that is a compiler barrier and needs no exclusive), or both.

## 7. Assembly at the top of a file cannot name a constant, so a frame layout is written twice

Milestone 1, the vector table. The entry code reserves a `Frame` and saves registers at its
offsets, so the asm needs `sizeof(Frame)` and `offsetof(Frame, elr)`:

    const FRAME: usize = 272

    asm
        [aarch64]
            "sub sp, sp, #{FRAME}"
        ...

    error: '{FRAME}' names an operand, and an 'asm' block at the top of a file has none. Write '{{'
    and '}}' for a brace the assembler is to see

Spec: inline-assembly.md § "Assembly at the top of a file" refuses every operand, and § "What an
operand may be" allows only a plain variable even inside a function, so there is no constant operand
anywhere.

Workaround (used): the offsets 272, 240, 248 and 256 are literals in the asm, and four `@assert`s in
`keel.trap` pin `sizeof(Frame)` and the field offsets to them. A changed field fails the build, but
the asm still has to be edited by hand to match. The same applies to every symbol the asm names
(`bl keel_trap`): a typo is a link error, not a sysl one.

Clean form: a constant operand (`sizeof`, `offsetof`, a `const`) and a symbol operand for a sysl
function in top-level asm, as Rust's `global_asm!` has `const` and `sym`. A context switch and a
syscall entry will need the same layout twice over.

## 8. A bitfield struct is one integer, but there is no conversion to or from that integer

Milestone 1, decoding `ESR_EL1`. The register is a `u64` read with `mrs`; `@packed` with `uN` fields
describes it exactly:

    @packed
    struct Esr
        iss: u25
        il: u1
        ec: u6
        iss2: u5
        res0: u27

    decode(raw: u64) -> Esr = Esr(raw)
    error: struct 'Esr' has 5 fields, but 1 value was given

    decode(raw: u64) -> Esr = Esr::Bits(raw)
    error: 'Esr' has no type attributes

    back(e: Esr) -> u64 = u64(e)
    error: cannot convert Esr to ulong

Spec: attributes.md § "Bitfields": "A packed struct whose fields all lower to an integer ... **is**
one unsigned integer". Nothing converts between the two.

Workaround (used, `keel.trap.syndrome`, where the struct is `Syndrome`): store the value and read
it back through a pointer:

    var word = raw
    val s: *Syndrome = ptr_cast(&word)
    *s

Every system register with fields (SPSR, SCTLR, TCR, the GIC's ICC registers, page-table entries)
will go through the same three lines.

Clean form: a conversion each way, such as `Syndrome(raw)`, `Syndrome::Bits(raw)` or `u64(s)`.

## 9. An enum field of a bitfield struct can hold a value no variant has: `Image` names the wrong variant and `match` traps

Milestone 1, the exception class. An ESR EC is six bits, about 25 of the 64 values are defined, and
the hardware may report any of them. The natural form makes the field the enum:

    enum Ec: u6
        Unknown = 0x00
        Brk = 0x3c

    @packed
    struct Esr
        low: u26
        ec: Ec
        high: u32

    var raw: u64 = u64(0x2a) << 26
    val e: *Esr = ptr_cast(&raw)
    print(Ec::Image(e.ec), u8(e.ec))

prints `Brk 42`, a wrong answer with no diagnostic. `e.ec match Unknown -> ... Brk -> ...` traps
(SIGTRAP on the host, exit 133, no message).

Spec: errors.md § "What traps" lists `Color(n)` on an undeclared discriminant as a checked cast that
traps, and attributes.md § "A simple enum" says `Image` does not trap. The bitfield read produces an
enum value without that check, so every later operation on it is wrong.

A fallible conversion does not exist either: `Ec(n)` is the checked cast and traps on 0x2a, so a
kernel cannot write "the class, if it is one I know" without listing every discriminant a second
time.

Workaround (used, `keel.trap.class_of`): the field is a plain `u6`, and the declared variants are
searched by position:

    for i in 0..ExceptionClass::Pos(ExceptionClass::Last)
        val c = ExceptionClass::Val(i)
        if u6(c) == ec then return Some(c)
    None

Clean form: check the discriminant where a bitfield field is read (or refuse an enum field whose
underlying width has values no variant covers), and add a fallible conversion, for example
`T::Try(n) -> Option[T]`.

## 10. System registers have no sysl surface: one asm function per register, eight lines each

Milestone 1 touches 13 system registers (CurrentEL, VBAR_EL1, ESR_EL1, FAR_EL1, DAIF, CNTFRQ_EL0,
CNTV_TVAL_EL0, CNTV_CTL_EL0, ICC_SRE_EL1, ICC_PMR_EL1, ICC_IGRPEN1_EL1, ICC_IAR1_EL1, ICC_EOIR1_EL1).
The register is part of the instruction text, and the text is a string literal, so each access is a
function of its own:

    esr() -> u64
        var v: u64 = 0
        asm
            [aarch64]
                "mrs {v}, esr_el1"
                out v : reg
            [x86_64, thumb, riscv64, riscv32, craft, wasm32] unavailable "keel runs on aarch64 only"
        v

There are 14 such blocks in keel, about 100 lines, of which 16 are instructions. Every block in a
module that only makes sense on AArch64 also repeats the `unavailable` arm (inline-assembly.md
§ "Every architecture needs an answer"), and nothing lets a module say once that it is AArch64's.

Not a blocker, since every register is reachable. Clean form: an `mrs`/`msr` intrinsic taking the
register as a constant (clang has `__builtin_arm_rsr64("esr_el1")` / `__builtin_arm_wsr64`, which are
`llvm.read_register` / `llvm.write_register` with a metadata name), or a constant string operand in
the template.

## 11. No hexadecimal formatting in the standard library (minor)

`print` writes integers in decimal only (`sysl.printu`), and `sysl.encoding.hex` encodes bytes, not
an integer. A register dump is read in hex. Workaround (used): `keel.pl011.put_hex`, twelve lines.

## Milestone 2: the MMU, with the kernel in the high half

Findings 12-17 are against the same prerelease alpha.5 (`census-34` at `7a59452170`), found while
writing the page tables, the boot path that runs before and after the MMU is switched on, and the
high-half kernel. By how much each blocks: 12 (silent, wrong tables), 15 (the build stops), 13, 16,
14, 17. None was fixed here.

## 12. `@align(n)` on a struct does not reach storage: a 4 KiB-aligned table type lands anywhere

Milestone 2, the translation table pool. A table is one 4 KiB page and the hardware ignores the low
twelve bits of its address, so the type says so:

    @align(4096)
    struct Table
        e: [512]u64

    var tables: [2]Table                // module storage
    var one: Table

    @align(4096)
    var raw: [512]u64                   // the binding form

    main()
        var local: Table
        print(sizeof(Table), alignof(Table))
        print(usize(&tables[0]) % 4096, usize(&tables[1]) % 4096, usize(&one) % 4096,
              usize(&raw) % 4096, usize(&local) % 4096)

prints

    4096 4096
    64 64 64 0 2272

`alignof` answers 4096, and no value of the type is on that boundary: the module globals are emitted
with no `align` at all (`@tables = private global [2 x %struct.Table] zeroinitializer`), and the stack
slot of a local is not aligned either. Only `@align` on the binding works
(`@raw = ... align 4096`).

Spec: attributes.md § "`@align(n)` on a binding": "A boundary put on a type travels with every value
of it ... so reach for the type when more than one thing needs the boundary." It does not travel.
Nothing diagnoses it; a kernel built this way hands the MMU a table address with low bits set, which
the walker silently truncates to the wrong table.

Workaround (used, `keel.mm.table`): `@align(4096)` repeated on `var pool: [16]Table`, with a comment.
`sizeof(Table)` being 4096 keeps every element aligned once the first is.

Clean form: the type's alignment on every global, stack slot and heap allocation of it, as LLVM's
`align` on the `global`/`alloca`.

## 13. A function that runs before the MMU cannot know which of two addresses `&x` gives it

Milestone 2, `keel_boot`. The image is linked at 0xFFFF_0000_4008_0000 and runs at 0x4008_0000 until
the MMU is on, so the boot code must take physical addresses of linker symbols and of its own module
storage. `&` gives it two different kinds of address, and the source cannot tell them apart:

    extern __bss_start: u8              // keel.boot
    @align(4096) private var pool: [16]Table    // keel.mm

    usize(&__bss_start)                 // the LINK address: 0xFFFF_0000_4008_5000, from the GOT
    usize(&pool[0])                     // the RUN address: 0x4008_6000 before the MMU, PC-relative

The compiled code, at -O1 and -O0 alike:

    keel.boot$keel_boot:
        adrp x8, 0xffff000040084000     // the GOT's page
        ldr  x8, [x8, #0x6f0]           // __bss_start's link address, read from the GOT
    keel.mm$invalidate_pool:
        adrp x8, 0xffff000040086000     // pool, PC-relative: wherever the code is running
        add  x8, x8, #0x0

An `extern` is emitted as `@__bss_start = external global i8` without `dso_local`, so even in this
static, non-PIC link LLVM reaches it through the GOT; a module variable is `private` and reached
PC-relative. Writing through `&__bss_start` with the MMU off faults; writing through `&pool[0]` works.
lld can also relax an adjacent `adrp`/`ldr` GOT pair into `adrp`/`add`, which would silently turn the
first kind into the second for whichever accesses the optimizer happens to schedule together.

There is nothing to say instead: no attribute marks a function as running at an address other than
its link address (`__attribute__((section(".idmap.text")))` plus `-fPIC` is how C kernels arrange
it), no form asks for a symbol's link address or its run address explicitly, and no diagnostic
notices an absolute address in code that will run before relocation.

Workaround (used, `keel.mm.phys_of`): mask the top sixteen bits. The kernel's VA is its PA plus
0xFFFF_0000_0000_0000, so `addr & 0x0000_FFFF_FFFF_FFFF` is the physical address whichever kind
`addr` is. It works only because keel chose that layout; a kernel at, say, 0xFFFF_FFFF_8000_0000
(Linux's kimage) could not use it.

Clean form: one of `@position_independent` on a function (refuse GOT loads and absolute relocations
inside it, and make `&extern` PC-relative), or `dso_local` on `extern` variables in a freestanding
image, so that `&` always means "where it is now".

## 14. Barriers and maintenance instructions have no spelling (`dsb`, `isb`, `tlbi`, `dc`, `at`)

Milestone 2. Turning the MMU on and changing a translation table needs `dsb` (with a domain:
`sy`, `ish`, `nsh`, `ishst`), `isb`, `tlbi vmalle1`, `dc ivac` over the tables written with the MMU
off, and `at s1e1r` to ask the MMU what a mapping is. `sysl.sync.atomic_fence` is the one barrier sysl
has, and it is a `dmb`: it orders accesses against accesses, not against the translation machinery.

So each is a function of its own, as finding 10's registers are:

    flush_tlb()
        asm
            [aarch64]
                "dsb ishst"
                "tlbi vmalle1"
                "dsb nsh"
                "isb"
            [x86_64, thumb, riscv64, riscv32, craft, wasm32] unavailable "keel runs on aarch64 only"

Milestone 2 added fourteen such functions to `keel.cpu` (107 lines of sysl around 21 instructions):
eight system registers (MAIR, TCR read and write, TTBR0, TTBR1, SCTLR read and write, ID_AA64MMFR0,
CTR_EL0, PAR_EL1 via `at`) and five barriers or maintenance operations.

Not a blocker. Clean form: clang's `__builtin_arm_dsb(n)`, `__builtin_arm_isb(n)` are intrinsics with
a domain argument; with finding 10's register intrinsic, a module like `keel.cpu` would be a list of
one-line functions. `tlbi`/`dc`/`at` take an operand and have no intrinsic in clang either, so an asm
template that may name a constant would cover them.

## 15. A module `val` holding any enum variant but the first is emitted as IR clang refuses

Milestone 2, the protection a mapping is made with. Shortest input:

    enum Wide
        A
        B

    struct P2
        w: Wide

    val W: P2 = P2(B)

    main()
        print(Wide::Image(W.w))

    sysl: error: clang failed (exit 1):
    ...ll:475:40: error: constant expression type mismatch
      475 | @W = private constant %struct.P2 { i32 { i32 1, [0 x i8] zeroinitializer } }

The enum's discriminant is written as if it were a data enum's `{ tag, payload }` in a slot whose type
is the bare integer. It is the same with an underlying type (`enum Access: u2`,
`val BARE: Access = Ro` gives `@BARE = private constant i2 { i32 2, [0 x i8] zeroinitializer }`), with
the enum bare or inside a struct, and only for a non-zero discriminant (the first variant is a
`zeroinitializer`, which is why `Normal` alone would have passed).

Spec: a constructor over literals is a module `val` that folds into the image (a freestanding image
depends on it, having no initializer to run); nothing says an enum value is an exception.

Workaround (used, `keel.mm.desc`): `ram_rw()`, `text()`, `device()` and the rest are functions
returning the `Prot` rather than module `val`s.

## 16. A bitfield struct may not contain a bitfield struct, so a descriptor's attribute group is written out in every descriptor

Milestone 2, the descriptors. A block and a page descriptor (and, later, a stage 2 descriptor) share
the same lower-attribute field group, bits [11:2], and the same upper group, [63:50]. The natural form
names the group once:

    @packed
    struct Lower
        attr: u3
        ns: u1
        ap: u2
        sh: u2
        af: u1
        ng: u1

    @packed
    struct Desc
        valid: u1
        page: u1
        lower: Lower
        oa: u36
        rest: u16

    error: 'Desc.lower' is Lower, and 'Desc' is '@packed' with a field narrower than a byte — so it is
    one integer, and every field of it has to be one too. Put the narrow fields in a '@packed' struct
    of their own and hold that here: it lays out identically and leaves Lower what it is

`Lower` is itself one integer (ten bits), which is what the rule asks of a field. The advice
describes what was written.

Spec: attributes.md § "Bitfields": "**Nesting is the composition path** ... a bitfield struct is a
leaf, and an ordinary `@packed` struct lays one out as a field of its size" — whole bytes, so a ten-bit
group at bit 2 has no spelling.

Workaround (used): one `LeafDesc` for both blocks and pages (they share a layout and differ in bit 1),
its seventeen fields written out; a stage 2 descriptor will repeat them again.

Clean form: a bitfield struct allowed as a field of a bitfield struct, occupying its own bit width.
The diagnostic should say that a bitfield struct cannot be nested, rather than advising what was done.

## 17. A distinct address type is spellable, but it is an integer with every integer operation (minor)

Milestone 2, `keel.mm.PhysAddr` and `VirtAddr`. `type PhysAddr = new u64` does what was wanted
first: a `PhysAddr` will not pass where a `VirtAddr` or a `u64` is wanted, and `new u36` works as a
bitfield field (`Frame`, the descriptor's output address). Three rough edges:

- `PhysAddr * PhysAddr`, `PhysAddr + PhysAddr` and the rest are inherited and cannot be taken away
  (errors.md § "A derivation inherits its base's behaviour and may replace none of it").
- An offset is `impl Add[u64] for PhysAddr`, which is allowed, but a literal does not take the
  extension's type: `p + 4096` is refused with `'+' needs matching types, got PhysAddr and int`, so
  every call site writes `p + u64(4096)`.
- A derived type has no inherent methods (`impl PhysAddr` is `'for' expected`), so the conversions
  are free functions, `to_virt(p)` rather than `p.to_virt()`.

A struct with one field would control all three, and would then not be a bitfield field. Not a
blocker. Clean form: a literal infers from the one `Add` argument a derived type has beyond its base,
and an inherent `impl` block for a derived type.

## Milestone 3: physical frames and the kernel heap

Findings 18-20 are against the same prerelease (`census-34` at `7a59452170`), found while writing the
frame allocator, the kernel heap that is sysl's allocating pair, and the C memory functions a
freestanding image owes. By how much each blocks: 18 (silent, a hang), 19 (the build stops), 20.
None was fixed here.

## 18. An exported `memset` written as a loop compiles to a call to itself

Milestone 3, `keel.rt`. A freestanding image defines `memset`, `memcpy`, `memmove` and `memcmp`
itself (getting-started/cli.md: "Freestanding does not mean self-contained ... `memcpy` and `memset`
for a structure assignment you never wrote"). LLVM emitted a `memset` call for the frame bitmap's
zeroing loop, so keel wrote one:

    @export("memset")
    mem_set(dst: *u8, c: int, n: usize) -> *u8
        for i in 0..<n
            dst[i] = u8(c)
        dst

At the default level it compiles to

    ffff000040082da0 <memset>:
        cbz  x2, 0xffff000040082da8 <memset+0x8>
        b    0xffff000040082da0 <memset>        // the loop, recognised as a memset, calls memset
        ret

an endless loop for any non-zero length, and nothing says so: the image links, and the first zeroing
loop the optimizer turns into a call hangs the kernel. The `memcpy` loop beside it happened not to be
rewritten into a `memcpy` call this time, which is luck, not a guarantee. C has the same trap and two
spellings for it: `-ffreestanding`/`-fno-builtin` for the file, or
`__attribute__((no_builtin("memset")))` on the function (LLVM's `"no-builtins"` function attribute).
sysl has neither, and does not emit the attribute for a function `@export`ed under a libc name on a
freestanding target.

Workaround (used, `keel.rt`): every store goes through a `*volatile u8`, which LLVM may not merge into
a call. One byte at a time, which costs nothing yet.

Clean form: a freestanding target marks a function exported as `memset`, `memcpy`, `memmove` or
`memcmp` with `"no-builtins"` (it is the implementation, so it must not call itself), or an attribute
such as `@no_builtin` for any function that has to be written without the library it implements.

## 19. Naming a sysl function as the program's allocating pair declares and defines one symbol twice

Milestone 3, `keel.heap`. packages.md § "One heap, and the package that names it" says the project's
own manifest may name the pair ("an application with its own arena"), and a freestanding target has
to. keel's heap is sysl, exported under the names the manifest gives:

    allocator {                             // kernel/package.hocon
      alloc = "keel_alloc"
      free  = "keel_free"
    }

    @export("keel_alloc")                   // keel.heap
    keel_alloc(n: usize) -> *u8
    @export("keel_free")
    keel_free(p: *u8)

The compiler emits a `declare` for the pair into the program's own module unconditionally, then the
`define` of the export, and clang refuses the IR:

    sysl: error: clang failed (exit 1):
    scratch/keel.elf.ll:6133:12: error: invalid redefinition of function 'keel_alloc'
     6133 | define ptr @keel_alloc(i64 %c.0) #99 {

(line 75 is `declare ptr @keel_alloc(i64)`). The pair is evidently meant to come from C beside a
package; an arena written in sysl, which is what a kernel or an application with its own arena
has, cannot be named. `@export("exit")`, which the standard library also declares (`extern exit` in
`platform.sysl`), works, because that declaration lives in the separately compiled standard module.

Workaround (used): the manifest names `keel_heap_alloc` and `keel_heap_free`, and `link.ld` defines
them as aliases, `keel_heap_alloc = keel_alloc;`. Free, but only because keel has a linker script.

Clean form: the pair's `declare` is skipped (or reused) when the compilation defines the symbol, or a
package may name a sysl function as its allocator directly.

## 20. A boxed value gets at most the allocator's 16-byte alignment, whatever its type asks for

Milestone 3, probing the kernel heap. keel's `keel_alloc` answers 16-byte aligned blocks, as malloc
does on AArch64. A box puts its three header words (memory.md § "What a heap object costs") in front
of the payload:

    @align(64)
    struct LineSized
        x: u64

    val c: &u128 = 7
    val a: &LineSized = LineSized(1)
    val b: &PageSized = PageSized([0; 512])     // @align(4096), [512]u64
    print(usize(&*c) % 16, usize(&*a) % 64, usize(&*b) % 4096, alignof(PageSized))

prints `0`, `24`, `88`, `4096`. A `u128` is placed on its 16-byte boundary (the header is padded),
so natural alignment up to the allocator's is honoured; an `@align(n)` above that is not. Part of
this is finding 12 (the struct's `@align` not reaching storage, fix in flight), but the heap half
does not follow from that fix: the allocating pair is `alloc(size)` and `free(ptr)` with no alignment,
so a 4 KiB-aligned type cannot be boxed on that boundary unless the compiler over-allocates and
rounds up (and remembers the original pointer for `free`), or the pair has an aligned form. Nothing
diagnoses it.

No workaround needed yet: what a kernel wants page aligned (tables, DMA buffers) it takes from the
frame allocator directly, never from a box.

Clean form: a box of a type whose alignment exceeds the allocator's guarantee is laid out on that
boundary, by over-allocation or by an optional `aligned_alloc` member of the `allocator` block — or
refused, naming the type and the allocator's alignment.

## Milestone 4: kernel threads, a preemptive scheduler, locks

Findings 21-26 are against the same prerelease (`census-34` at `7a59452170`), found while writing
`keel.sched` (threads, the context switch, a round-robin scheduler preempted by the timer, `IrqLock`,
`WaitQueue`, a blocking `Mutex[T]`) and its boot tests. Ranked: 21 and 22 are silent corruption of
reference counts once threads are preemptive; 23 and 24 are silent leaks or a lock that stays held;
25 has a cheap workaround; 26 is cosmetic. None was fixed here.

## 21. The per-thread drain list the runtime asks a scheduler for cannot be supplied from sysl

Milestone 4, `keel.sched`. memory.md § "On a freestanding target the worklist is asked for rather
than assumed" says a release that reaches zero fetches the running task's drain list through a weak
`__sysl_arc_reaper`, and a port that schedules defines it ("Define it in an RTOS port and `&sync T`
means on that target what it means everywhere else"). keel is such a port: a thread preempted half
way through draining would otherwise have its list taken over by the next thread. Written:

    struct Reaper
        head: *u8
        draining: u8

    @export("__sysl_arc_reaper")
    arc_reaper() -> *Reaper = &this_cpu().current.reaper

    sysl: error: clang failed (exit 1):
    p1.elf.ll:6352:12: error: invalid redefinition of function '__sysl_arc_reaper'
     6352 | define ptr @__sysl_arc_reaper() #99 {

The weak default is emitted into the program's own module (`define weak ptr @__sysl_arc_reaper()`
returning `@arc.reaper.self`), so a sysl definition is a second `define` of the same name in the same
IR — finding 19's shape, for a hook whose only documented implementation is C. Every release path
calls it (`arc.reap`), plain `&T` as well as `&sync T`.

Without a definition nothing fails: one global list serves every thread, and a preemption inside a
drain corrupts it later.

Workaround (used): keel.sched exports `keel_arc_reaper` and `link.ld` assigns
`__sysl_arc_reaper = keel_arc_reaper;`, which wins over the weak definition (the two `bl`s in the
image go to keel's). Free, but only because keel has a linker script.

Clean form: the weak default goes into a separately compiled object (as `extern exit` does, finding
19), or is not emitted when the program defines the symbol; or the hook is named in the manifest
(`runtime { reaper = "..." }`), like the allocating pair.

## 22. The `&sync` capture check stops at module storage: a `&T` in a module `var` reaches every thread

Milestone 4, probing what a spawned closure may touch. The check on a `&sync Fn` is good (see "What
worked"): capturing a `&Cell` is refused at the closure. But the same `&Cell` held in module storage
is reached from two threads with no complaint:

    private struct Cell
        n: u64

    private var shared: Option[&Cell] = None

    module_probe()
        shared = Some(Cell(2))
        join(spawn(() -> touch_shared()))
        join(spawn(() -> touch_shared()))

    private touch_shared()
        shared match
            Some(c) -> c.n += 1               // a non-atomic retain and release, from either thread
            None -> ()

builds for aarch64-freestanding with no diagnostic. The `match` takes a share of the non-atomic count
on each thread, which is exactly the race memory.md § "Crossing a concurrency domain" says `&T` may
not have ("a `&T` may not leave its domain"). declarations.md says a module `var` "is one object the
whole program shares, which is what makes it the thing two threads can disagree about", and stops
there; `@thread_local` is the only module-storage rule.

A kernel's shared state is module storage (keel's run queue, per-core records, allocator state), so
this is the case that matters most there. keel's own globals hold only raw pointers, scalars and
`sysl.sync` types, by care rather than by check.

Clean form: a module `var` whose type reaches a non-atomic count is refused (or warned) when a body
reachable from a `&sync Fn` or a `@crossing` parameter names it — or, more simply, a module `var` in
a program that ever crosses a domain must hold only crossable types, `@thread_local` excepted.

## 23. A frame abandoned by a context switch keeps every count it holds, silently

Milestone 4, the thread entry. A thread ends by calling `exit() -> never`, which switches away and is
never switched back to; its stack is freed. Written the natural way:

    thread_entry(t: *Thread) -> never
        ...
        t.entry match
            Some(f) ->
                f()
                exit()                       // the share of `f` the match took is never released
            None -> exit()

the boot test reports `keel: threads leaked: ... blocks live 16 of 3`: thirteen heap blocks (the
closures and what they captured) for the nine test threads. No diagnostic: calling a `-> never` function is
treated like a trap, which statements.md § `defer` says "runs nothing". That is right for `panic`; a
kernel's `exit` is the ordinary end of a thread's life, and the frame is gone for good.

The same rule shapes the scheduler: every local holding a count across `keel_switch` is held for as
long as that thread is switched out, and for ever by a thread that exits. So the run queue and the
wait queues are intrusive lists of raw `*Thread` linked through a `next` field, and a thread keeps its
own `&sync Thread` box alive through a `me` self-reference that `join` drops — the counted
alternative (`next: Option[&sync Thread]`) would put retains and releases on the switch path, where
this finding makes them leak.

Workaround (used): the closure is called from a helper, `run(t)`, that returns before `exit()` is
called, so the frame that calls `exit` holds nothing counted; the scheduler's queues hold raw pointers.

Clean form: a way to say a call abandons the frame (an attribute on `exit`, or a `become`-like form)
so the frame's counts are released and its `defer`s run before it; or at least a warning when a frame
holding counts calls a `-> never` function that is not a trap.

## 24. A lock guard's `Drop` never runs, and the warning the reference promises for it is not emitted

Milestone 4, `Mutex[T]`. The natural guard is a value whose destructor unlocks:

    struct MGuard
        m: *Mutex[u64]

    impl Drop for MGuard
        drop(self)
            print("probe: guard dropped")
            self.m.unlock()

    guard_of(m: *Mutex[u64]) -> MGuard
        val _ = m.lock()
        MGuard(m)

    guard_probe()
        var m = mutex(u64(0))
        if true
            val g = guard_of(&m)
            print("probe: guard held", usize(g.m) != 0)
        print("probe: scope left")

prints `probe: guard held true`, `probe: scope left`, and never `guard dropped`: the mutex stays
locked. That much is the documented model (memory.md: a destructor runs when a *box's* count reaches
zero), and boxing a guard costs an allocation per lock, which `@no_alloc` code such as the IRQ path
cannot make. But memory.md also says "The compiler says so now": `warning: 'open_bad' hands back
'Encoder' by value, and 'Encoder' has a 'Drop' ...`. The self-hosted prerelease emits no such
warning for `guard_of`, nor for memory.md's own `open_bad() -> Encoder` built for the host — a
documented check that is absent (a bug, not a gap).

So keel's `Mutex[T]` is `lock(*self) -> *T` plus `defer m.unlock()`, as `sysl.posix.threads.Mutex`
is. It works, and nothing ties the `*T` to the hold: `val p = m.lock(); m.unlock(); *p += 1`
compiles.

Workaround (used): `defer m.unlock()` beside every `lock`; `IrqLock.lock()` answers the saved
interrupt mask, which the caller passes back to `unlock`.

Clean form: the warning restored; and a scoped form for a guard — a value whose `Drop` runs at the
end of its block when it was never boxed or moved, or a `with`-style borrow that hands the body a
reference which cannot outlive it.

## 25. `@thread_local` is refused on a freestanding target even where the program owns the thread pointer

Milestone 4, per-thread data. keel writes TPIDR_EL1 itself (each core's record) and could lay down a
TLS block per thread and point a thread register at it on every switch. sysl does not let it say so:

    @thread_local var per_thread: u64 = 0

    error: 'keel.probe.per_thread' cannot be '@thread_local' on 'aarch64-freestanding': there is no
    loader and no libc on this target, so nothing lays a thread's storage down and nothing writes the
    thread pointer the offset would be read from. Declare it as a plain module 'var' and let the port's
    scheduler answer for which task is running, which is what the ownership runtime's reaper slot does

The message's advice is what keel does, and it works: per-thread data are fields of `Thread`, reached
through `sched.current()` (one `mrs tpidr_el1` and two loads). But a kernel *is* the thing that
writes the thread pointer, and has no way to tell the compiler which register (LLVM's
`-mtp=el1`/`tpidr_el1`) or how big a thread's block is (the `.tdata`/`.tbss` image size), so every
per-thread variable is a field added to one struct in the scheduler.

Clean form: a target or manifest key that says the program supplies the thread pointer (register,
and the symbols bounding the TLS image), after which `@thread_local` compiles to TPIDR-relative
accesses and the scheduler copies the image per thread.

## 26. An exported function may not be `private`, though its only caller is assembly in the same module (minor)

    @export("keel_thread_entry")
    private thread_entry(t: *Thread) -> never

    error: 'keel.sched.thread_entry' is private, which promises every caller is inside its own module —
    and an export promises the opposite. A definition cannot make both claims

Its only caller is `keel_thread_start`, top-of-file asm in `keel.sched` itself, which reaches sysl only
through a symbol. The same holds for `keel_trap` and `keel_arc_reaper`. Workaround: not `private`, so
the module's public surface carries three functions nothing in sysl may call.

Clean form: an `@export` may stay `private` (the symbol is for the linker, not for sysl), or the
module's own asm may name a private function's symbol.

## Milestone 5: multi-core

Findings 27-30 are against the same prerelease (`census-34` at `7a59452170`), found while starting
QEMU `virt`'s three secondary cores with PSCI `CPU_ON` (`keel.smp`), giving each its redistributor,
timer and `Cpu` record, sending reschedule IPIs, and auditing the milestone-4 locks for four cores.
Ranked: 29 is a miscompile (sizes and offsets the generated code does not use); 27 is a class of
silent per-core bug the language cannot mark, one of which milestone 4's code had; 28 is what the
`Atomic` orderings cannot say on aarch64, most of it addressed by the system-register and barrier
built-ins in flight; 30 is a missing spelling with a hand-padding workaround. None was fixed here.

## 27. Per-core data has no storage class, and nothing marks the window in which reading it is right

Milestone 5, `keel.sched`. A core's record is reached through TPIDR_EL1, which the kernel writes, so
"this core's data" is a pointer the program keeps rather than storage the compiler knows anything
about (`@thread_local` is refused on the target, finding 25, and would be per *thread* anyway). That
is workable, but the read has a property sysl has no way to state: it is right only while the thread
cannot move to another core. Milestone 4's ownership-runtime hook was written the natural way:

    @export("keel_arc_reaper")
    arc_reaper() -> *Reaper
        val c = cpu.tpidr()                         // core A's record
        ...
        &p.current.reaper                           // preempted in between: resumed on core B,
                                                    // this is now core A's *next* thread's list

It is called on every release that reaches zero, from any thread, with IRQs on. With one core it was
correct; with four, a preemption between the `mrs` and the load hands a thread another thread's drain
list — the corruption finding 21 exists to prevent, reintroduced by migration, with no diagnostic and
no failing test (the window is two instructions). `sched.current()` and `trap.probe` had the same
shape. Every function reaching per-core data is suspect in the same way, and nothing distinguishes
`this_cpu().slice` (right: only called in IRQ context) from `this_cpu().current` read in a thread.

The `&sync` check (finding 22) cannot help either: the record is reached from a register, through a
raw pointer, so it is outside every check by construction.

Workaround (used): every read of TPIDR_EL1 outside IRQ context is bracketed by `irq_save` /
`irq_restore` (`arc_reaper`, `current`, `trap.probe`), with a comment on `this_cpu` saying so; the
`Cpu` records are written only by their own core or under the scheduler's lock. Linux's answer is the
same discipline plus `this_cpu_*` accessors that are one instruction; keel's costs two `msr`s per
release-to-zero.

Clean form: a per-core storage class (`@per_core var`, the program naming the register and where each
core's block is, as finding 25 asks for threads), whose accesses the compiler either makes single
instructions or refuses outside a scope that has disabled preemption (`with no_preempt` or a function
attribute the IRQ path and the scheduler carry) — the same shape as `@no_alloc`: a property of a call
tree, checked.

## 28. The `Atomic` orderings are C11's, and three things an aarch64 kernel needs are outside them

Milestone 5, the memory-ordering audit. What the orderings say, read off the image (`-O1`, no LSE):

| sysl | aarch64 |
|---|---|
| `SpinLock.lock` (`swap`, `Acquire`) | `ldaxr` / `stxr` loop, then a relaxed `ldr` spin |
| `SpinLock.unlock` (`store`, `Release`) | `stlr` |
| `load(Acquire)` / `load(SeqCst)` | `ldar` |
| `add(Relaxed)` | `ldxr` / `stxr` |
| ARC release-to-zero (acquire fence) | `dmb ishld` |

All of that is right, and it was enough for every lock in keel: `IrqLock`, `Mutex` (its value is
reached between two guard acquisitions, so the guard's release/acquire pair orders it), `WaitQueue`,
the run queue's handover (the switched-from thread's saved context is published by the next thread's
`stlr` on the lock), the frame allocator and the heap. What they cannot say:

1. **Memory against a system register write.** A reschedule IPI is `msr icc_sgi1r_el1`, which is not
   a memory access; no C11 ordering, fence included, orders the run-queue store before it. It takes a
   `dsb ishst` (Linux's `gic_ipi_send_mask` has the same one). keel happens not to need it — the woken
   core takes the run-queue lock, whose acquire pairs with the sender's release — but the language gave
   no way to say it, so it is in the asm block beside the `msr`. Translation-table writes before a walk
   (`dsb ishst; isb`) and TLB maintenance (`tlbi …is; dsb ish`) are the same: memory ordered against
   the MMU, not against another load.
2. **The shareability domain.** `atomic_fence` is always `dmb ish`/`ishld`. Ordering a store to Normal
   memory against a device write wants `dmb osh` (or `dsb`), which no ordering expresses.
3. **Which instructions.** No `codegen` key selects LSE (`'codegen.lse' is not a code generation
   setting — the settings are 'general_regs_only' and 'strict_align'`), so every RMW is an LL/SC loop
   even on a host core that has `swpa`/`ldadd`; and `SpinLock`'s spin is a bare `ldr; cbnz` — no
   `wfe`/`sevl` (the aarch64 idiom, woken by the holder's `stlr` clearing the monitor) and no `yield`.
   `sysl.sync` has no spin-wait hint for a library lock to use.

Items 1 and 2 are finding 14's (barriers have no spelling), and the prerelease in flight adds
`dsb(opt)`, `dmb(opt)` and `isb()` built-ins; this records *why* the orderings alone are not enough,
so that the built-ins are documented as the answer rather than as a lower tier.

Workaround (used): `gic.send_sgi` is one asm block, `dsb ishst; msr icc_sgi1r_el1; isb`; the TLB and
table barriers stay keel's asm wrappers.

Clean form: the barrier built-ins (in flight), with `sysl.sync`'s page saying which orderings C11
leaves to them; a `codegen` key for the target's atomic instructions; a spin-wait hint in `sysl.sync`
that `SpinLock` uses.

## 29. `@align(n)` on a struct changes `sizeof` and `offsetof` but not the generated layout

Milestone 5, the per-core records. Each core's `Cpu` should own a cache line, so the type says so:

    @align(64)
    struct Cpu
        id: u64
        slice: u64

    struct Pair
        a: Cpu
        b: Cpu

    @align(64)
    var cpus: [4]Cpu

    probe()
        var local: [4]Cpu
        print("sizeof", sizeof(Cpu), "alignof", alignof(Cpu), "sizeof [4]Cpu", sizeof([4]Cpu))
        print("module stride", usize(&cpus[1]) - usize(&cpus[0]), "base mod 64", usize(&cpus[0]) % 64)
        print("local stride", usize(&local[1]) - usize(&local[0]))
        print("Pair: sizeof", sizeof(Pair), "offsetof b", offsetof(Pair, b))
        var p = Pair(Cpu(1, 2), Cpu(3, 4))
        print("Pair field distance", usize(&p.b) - usize(&p.a))

prints (host, `sysl run`):

    sizeof 64 alignof 64 sizeof [4]Cpu 256
    module stride 16 base mod 64 0
    local stride 16
    Pair: sizeof 128 offsetof b 64
    Pair field distance 16

The compile-time layout pads `Cpu` to 64 bytes; the emitted code lays it out at 16. `sizeof`,
`offsetof` and `@assert`s about them describe a struct the program does not have — a `memcpy` of
`sizeof(Cpu)` bytes copies four elements, an asm offset taken from `offsetof` (finding 7's pattern)
reads the wrong field, and every element after the first shares a line with its neighbours, which is
what the annotation was written to prevent. Nothing diagnoses it. This is not finding 12 (the
alignment of storage): there the size already equalled the alignment, so only the base address was
wrong; here the size itself disagrees between the analysis and the code.

Workaround (used): no `@align` on `Cpu`. Its fields happen to add up to 64 bytes, an
`@assert(sizeof(Cpu) == 64, …)` holds them to it, and the `cpus` binding carries `@align(64)` (which
works, finding 12). A field added later needs hand padding.

Clean form: a struct's alignment padded into its LLVM type (a trailing `[N x i8]`), so the IR's
size and stride are the ones `sizeof` reports.

## 30. A field cannot ask for alignment, so a lock word cannot be given a cache line of its own (minor)

Milestone 5, false sharing. `Mutex[T]`'s guard word sits beside the value it protects and the fields
of whatever struct holds the mutex; on four cores, the spinning waiters' loads share the line with
the holder's writes. The C answer is an aligned member:

    struct Guarded
        lock: u32
        @align(64)
        value: u64

    error: the only annotations a member may carry are the ones about a parameter — '@crossing',
    '@borrows', '@reads' and '@writes' — and the three about the function it lowers to, '@noinline',
    '@inline' and '@cold'. ... A member is neither, so it goes above a free function instead ('06')

(The field is read as a member and refused with a member's rule — the message is about functions.)
With finding 29, the only way left is a `pad: [60]u8` field sized by hand, which a change of a field's
width silently breaks.

Workaround: none needed yet — the scheduler's lock and the run queue share a line on purpose (both
are taken together), and keel's `Mutex` is not hot. Recorded for when it is.

Clean form: `@align(n)` on a field (C's `_Alignas` on a member), and finding 29 fixed so the struct's
size follows.

## What worked in milestone 5

- **Starting a core is one PSCI call and six instructions of module-level asm.** `psci.cpu_on`
  pins four inputs and an output to `x0`-`x3` (`in id : "x0"`, …, `out status : "x0"`), the same
  register in and out as two variables, which the asm form accepts. `keel_secondary_start` zeroes
  TPIDR_EL1, takes the boot stack from x0, calls `keel_secondary_boot` (sysl, MMU off, PC-relative
  only — finding 13's rule, which held) and jumps to `keel_secondary_main` in the high half. Both
  sysl halves are `@export`s that cannot be `private` (finding 26).
- **Per-core setup is sysl**: the redistributor found by matching GICR_TYPER's affinity against
  MPIDR, its wake-up, the CPU interface, the timer PPI, the reschedule SGI, the timer, the `Cpu`
  record, and an idle thread made of the code already running on the boot stack.
- **The milestone-4 locks needed no change for four cores.** The run-queue handover, the
  wait-queue lock order and the `Mutex` handoff were already written as multi-core arguments; the
  boot tests pass under HVF and TCG with 4 cores, and the controls fail: a `Mutex` that does not
  lock totals 399,779 of 400,000 across cores; the heap without its lock takes translation faults
  during the four-core churn; without reschedule IPIs the 400,000 additions finish on one
  core (`cores 1`) before any idle core's tick notices the work.
- **Dropping acquire/release does not show under HVF.** The `Mutex` guard rewritten with `Relaxed`
  swaps and stores passed five runs out of five, total 400,000 each: the critical section is long
  next to the window, and the `msr daif` asm blocks around it are compiler barriers. The orderings
  are right by argument, not by this test.

## What worked in milestone 4

- **`&sync Fn() -> unit` is the right type for a thread's body, and its capture check is exact.**
  Capturing a `&Cell` is refused at the closure ("the 'c' it captures reaches a '&keel.selftest.Cell',
  whose count is not. Hold it as a '&sync keel.selftest.Cell'"); capturing a `&sync` struct holding
  `Atomic`s, arrays and plain fields works, and mutating through it works. The check also looks
  inside a generic: `&sync Mutex[&Cell]` is refused at its declaration because the mutex's private
  `value` reaches a `&Cell`. `@crossing(f)` on `spawn` adds nothing once `f` is a `&sync Fn`: removed,
  the same closure is refused the same way.
- **The context switch is 17 instructions of top-of-file asm** called through an ordinary
  `extern "keel_switch" switch_context(from: *Context, to: *Context)`, plus 3 for a new thread's first
  return; `Context`'s offsets are `@assert`ed as `Frame`'s are (finding 7 still means writing them
  twice). Everything else — run queue, lock handover, preemption, wait queues, `Mutex[T]`, stacks with
  guard pages mapped into a window of their own — is sysl.
- **`@no_alloc` on `keel.trap` checked the whole preemption path**: `preempt`, `switch_locked`,
  `finish_switch` and the wait-queue wakes are reached from the IRQ handler and proven allocation-free.
- **`defer m.unlock()`** inside a loop body releases on every iteration, which is what the mutex test's
  40,000 locks rely on.
- A thread's `&sync Thread` box, its stack from the frame allocator, and its closure all come back once
  it is joined (`keel: threads reclaimed`), and the controls show each half: no `free_stack` leaves 36
  frames and 9 stacks behind; finding 23's natural `exit` leaks 13 blocks.
- `sysl.sync.Atomic` and `SpinLock` were enough for every scheduler structure; the only new asm
  wrappers are TPIDR_EL1 (read, write), MPIDR_EL1, a broadcast `tlbi vaae1is`, a table-publishing
  `dsb ishst; isb`, and `yield` (findings 10 and 14's pattern, six functions, ten instructions).

## What worked in milestone 3

- **sysl's own allocation in the kernel**: `Buf[u64]` grown to 10,000 elements, an interpolated
  `string`, a `&T` box captured by a closure, all through `keel_alloc`/`keel_free`, and every frame
  back afterwards. The `allocator` block was the whole wiring (plus finding 19's alias); the standard
  module is rebuilt for the pair on demand. The `heap` capability needed nothing: a manifest that does
  not mention it provides it.
- `panic` and a failed `unwrap` end in `exit`, an ordinary C symbol: `@export("exit")` in `keel.psci`
  prints and powers off, so out of memory is a panic with a message.
- `@no_alloc` on `keel.trap` is checked **through every call**: a `s"..."` added to `keel.irq.dispatch`
  was refused at the handler's call to it ("this reaches 'keel.irq.dispatch', which makes heap
  storage"). The exception path is proven allocator-free. Releasing a reference is still allowed there
  and reaches `keel_free`, which is why the heap masks IRQs around its lock.
- `sysl.sync.SpinLock` is the lock for both allocators, with IRQs masked through two small asm
  functions (`cpu.irq_save`/`irq_restore`).
- `Option[Frame]` where `Frame = new u36`: a distinct, bitfield-sized frame number in an `Option`,
  with no ceremony.
- The device tree is read with `blob[0..<n]` over a `*u8`, `sysl.encoding.get_u32_be`, and slice
  equality against a string's bytes (`b[at..<at + s.len] == s.bytes`): about sixty lines, no parser.
- A module `var` array of raw pointers (`[8]*Page`) starts as nulls, and `null` compares against a
  field of a raw struct pointer as expected.

## What worked in milestone 2

- Page tables entirely in sysl: `TableDesc`, `LeafDesc`, `Tcr`, `Sctlr`, `Ttbr` and `Par` are
  `@packed` bitfield structs with `@assert(sizeof(...) == 8)`, and enum-valued fields (`MemAttr: u3`,
  `Access: u2`, `Shareability: u2`, `Cacheability`, `Granule0`/`Granule1`) are written by name. Two
  enums with variants of the same name (`Granule0.Kb4 = 0`, `Granule1.Kb4 = 2`) resolve by the field's
  type in a constructor. Finding 8's round trip through memory is two generic functions,
  `cpu.to_word[T]` and `cpu.from_word[T]`, with the size `@assert`ed per instantiation.
- `type Frame = new u36` as a bitfield field: the output address is a frame number that cannot be
  confused with an address.
- The boot path is one sysl call: `_start` sets a physical stack, `bl keel_boot`, adds the answer to
  SP and branches to `kmain`'s link address (7 instructions; milestone 1 had 4). Everything before
  the switch, including the table walk and the pool, is ordinary sysl that happens to compile to
  PC-relative code — at -O1 and -O0, under HVF and TCG.
- A linker script with `AT()` per section: QEMU loads by physical address and adjusts the ELF entry
  point to `_start`'s physical address by itself.
- `sysl.sync.Atomic` with the MMU on: `ticks.add(1, Relaxed)` is an `ldxr`/`stxr` loop on Normal
  memory and works (finding 6's fault is gone). The trap handler's probe flags are atomics too.
- A data abort the handler steps over: `*volatile u64` from `ptr_cast` is one `ldr`, so `elr += 4`
  resumes after it.

## What worked in milestone 1

- The vector table as top-of-file asm: `@align(2048)` and `@section(".text.vectors")` placed it at
  0x4008_1000, and `.irp slot, 0, ..., 15` with `"mov x1, #\\slot"` gave the 16 slots their indices.
- `@export("keel_trap")` gives the asm a fixed symbol to `bl` to, and the handler changing
  `frame.elr` changes where `eret` goes. At -O0 the published symbol is an entry that calls the
  definition, which is fine for a callee.
- `@assert(offsetof(...))` caught a real mistake: a reserved array in the GICv3 redistributor was
  sized for 0x400 bytes instead of 0x2fc, and the build refused it with "the left side is 1280".
- `[32]volatile u32` arrays in a register struct, and GICv3 system register names
  (`icc_iar1_el1` and the rest) in asm, all accepted.
- `ExceptionClass::Image` names a fault in the trap line, so a data abort prints
  `keel: trap DataAbort` with no table of strings.

## What worked in milestone 0

- volatile u32 struct over ptr_cast(0x0900_0000): exact ldr/str, the poll loop is not hoisted.
- `extern __bss_start: u8` / `&__bss_start`; .bss is zeroed in sysl, no memset emitted.
- `asm "hvc #0"` with `in id : "x0"`: HVF answers PSCI SYSTEM_OFF and QEMU exits 0.
- @export("putchar") in keel.pl011 is picked up through the standard library's extern seam, so print works.
- A bounds trap calls the weak `sysl_bounds_fail` (a ret stub), then brk #1; a kernel can supply its own.
- No memcpy/memset calls in any freestanding object so far.
