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

## Milestone 6: user mode

Findings 31-35 are against the newer prerelease `1a7aee7358` (module-level `volatile`, the
`read_sysreg`/`dsb`/`isb` built-ins, dso_local externs on freestanding), which keel moved to at the
start of the milestone after the milestone-5 boot test passed with it unchanged. Found while giving
keel processes (`keel.proc`: a TTBR0 table and an ASID each, an ELF loader, `eret` to EL0 and back,
four system calls) and writing five user programs in sysl (`user/`, a workspace beside the kernel).
Ranked: 31 is silent — module storage the image never fills, read as zeros, found as a jump to
address 0; 32 is the type a kernel most wants at its system-call boundary; 33 is how a kernel carries
its first programs; 34 extends finding 23 to the one place a kernel abandons frames by design; 35 is
minor. None was fixed here.

## 31. Module storage whose initializer does not fold is filled by a `main` nothing calls, silently

Milestone 6, the system-call table. The natural dispatch is a table of the handlers' addresses:

    private type Call = *extern(*Process, *u64) -> i64
    private val calls: [5]Call = [&sys_unknown, &sys_exit, &sys_write, &sys_getpid, &sys_brk]

    system_call(regs: *u64)
        val f = if regs[8] < 5 then calls[usize(regs[8])] else calls[0]
        regs[0] = u64(f(current_process(), regs))

It builds with no diagnostic, and the first `svc` from EL0 jumps to address zero:

    keel: trap InstructionAbort esr=0x0000000086000006 elr=0x0000000000000000 far=0x0000000000000000

The table is not static data. `sysl emit-llvm` shows `@keel.proc$calls = private global [5 x ptr]
zeroinitializer`, and a `define dso_local i32 @main` storing the five addresses into it — a `main`
the image never runs, because its entry is `_start` (packages.md § "When the image has a `main`": "calling
it is the entry's job"). The same `main` already held two stores keel never noticed, because both
happen to write zeros: `boot_maps = BootMaps(PhysAddr(0), PhysAddr(0))` (milestone 2) and
`empty_root = PhysAddr(0)` (this milestone) — a constructor of a `new` type does not fold either.

And keel has no point at which calling that `main` would be right. Before the MMU is on (in `_start`),
`&sys_write` computes PC-relative to the *physical* address, which faults once the identity map is
dropped (finding 13's rule, now for function addresses). After it (in kmain), `main` would overwrite
`boot_maps`, which `keel_boot` filled with the MMU off and the secondaries read.

C puts a function's address in `.data` with a relocation, and a constant struct in `.rodata`; the
linker resolves both. Spec: packages.md says `main` is emitted "where there is something to do" and
that an image with no top-level statements, no declared `main` and "no module `val` computed at run
time" gets none — it does not say which initializers are computed at run time, and nothing reports
that one is in an image that will never run it.

Workaround (used): the dispatch is a `match` on the number, with no table.

Clean form: an address of a function, and a constructor of a `new` type or a struct over constants,
fold into the image like any other constant; and an image whose `entry` is not `main` but whose
`main` has work in it is refused (or warned), naming the storage — the `build-c` refusal quoted in the
org notes ("module storage an initializer fills before the program's own statements run. This artifact
has no entry point to fill it in") applied to a linked freestanding image.

## 32. No user-pointer type: a distinct type cannot take a parameter, and nothing marks a pointer's address space

Milestone 6, `write(fd, buf, len)`. A system call's pointer argument is an address in *another*
address space (the process's), which the kernel must check before it reads a byte. What sysl offers is
a distinct integer, and it does the important half:

    type UserAddr = new u64

    val b = *a           error: '*' needs a pointer or a reference, not UserAddr
    val q: *u8 = ptr_cast(a)
                         error: 'ptr_cast' reads an address, and ulong is not address-width —
                         convert it first, as 'ptr_cast(usize(n))'

so a `UserAddr` cannot be read through by mistake. (The second message names the base, `ulong`, not
`UserAddr`.) What it lacks:

- **A typed one.** `UserPtr[T]`, so `copy_from_user[T]` knows the size and a `*Stat` argument stays
  distinct from a `*u8` one, is refused:

      type UserPtr[T] = new u64
      error: 'UserPtr' takes type parameters, and only a transparent alias may — a 'new', 'within',
      'where', 'wrapping' or '@export' type is one type of its own, with one range, one check and
      one name in a header, so there is nothing for an argument to fill

  A generic `struct UserPtr[T]` with one `u64` field is the fallback; it gives up the one-word
  `new`'s arithmetic and conversions.
- **Anything that knows the address belongs to another space.** `ptr_cast(usize(u64(a)))` launders
  it in one line, and the control below is exactly that line. An ordinary `*T` built from a
  register value is indistinguishable from one into kernel memory; Linux's `__user` is a checker
  annotation (sparse) for the same reason.
- **A fault-tolerant copy.** The check keel uses walks the process's own tables (`mm.user_translate`)
  for every page of the buffer, then reads each through the kernel's linear map. The other design, a
  copy that may fault and whose fault resumes at a fixup (Linux's exception table), needs a way to
  name the faulting instruction's address from sysl and attach a recovery to it; `trap.probe` fakes
  one load of it with a volatile load the handler steps over.

Control: with `VALIDATE_USER_POINTERS = false` the write reads `buf` directly, and the bad-pointer
program's first call brings the kernel down — `keel: trap translation fault level 2 ...
far=0x0000000000001000`, power off, no `keel: bad pointer rejected`.

Workaround (used): `UserAddr = new u64` for the argument, a page-table walk before any byte is read,
and the bytes read through `to_virt` of the physical address the walk found — so the kernel never
dereferences a user address at all.

Clean form: a distinct type may take type parameters (`type UserPtr[T] = new u64`); and, further, a
pointer qualifier for another address space that `ptr_cast` refuses to drop without a named
conversion.

## 33. A file's bytes cannot be named; `.incbin` works, against the directory the build was started in

Milestone 6, carrying the user programs. The kernel has no file system yet, so the programs' ELF
images go in its read-only data. sysl has no `include_bytes`. Assembly at the top of a file has the
assembler's `.incbin`:

    @section(".rodata.programs")
    asm
        [aarch64]
            ".balign 8"
            ".globl keel_prog_hello, keel_prog_hello_end"
            "keel_prog_hello:"
            ".incbin \"scratch/user/hello.elf\""
            "keel_prog_hello_end:"
        ...

    extern keel_prog_hello: u8
    extern keel_prog_hello_end: u8

    hello() -> []const u8 = image(&keel_prog_hello, &keel_prog_hello_end)

That works, with three costs. The path is resolved against the working directory of `sysl build`, not
the source file: the same build started from `/tmp` fails

    <inline asm>:19:9: error: Could not find incbin file 'scratch/user/hello.elf'
    error: cannot compile inline asm

and `--include-path <dir>` does not reach the assembler (same error), so `scripts/boot-test.sh` `cd`s to
the repository's root first. The bytes are reached as two `extern`s and a subtraction, per image. And
the build does not know the kernel depends on the file — it happens to recompile every time (a changed
`hello.elf` did reach the next kernel build's `.rodata`), so nothing goes stale today, but nothing
says it will not.

Workaround (used until milestone 10): the `.incbin` block above, one pair of labels per program,
built from the root. Since milestone 10 keel reads every program from its filesystem's `/bin`, so
the block is gone and the kernel builds from any directory; the gap itself is still open for a
kernel that has to carry a first program (an initramfs) or any other blob.

Clean form: `embed("path") -> []const u8` (Rust's `include_bytes!`, C23's `#embed`), a constant
resolved against the file it is written in, with alignment, which the build tracks as an input.

## 34. Leaving user mode abandons every frame between the exception and the entry, and nothing says so

Milestone 6, `keel.proc`. A process's thread enters EL0 from `run` and comes back only when the
process ends — by `exit`, a fault, or `kill` — from inside an exception handler three or four frames
deeper on the same kernel stack (the vector code, `keel_trap`, `user_sync`, the system call). The
return is a `longjmp`: `keel_leave_user` (module-level asm) restores the callee-saved registers and SP
that `keel_enter_user` saved, and `enter_user` "returns" a second time with the status.

    extern "keel_enter_user" enter_user(kernel: *sched.Context, entry: u64, sp: u64, arg: u64) -> u64
    extern "keel_leave_user" leave_user(kernel: *sched.Context, status: u64) -> never

Everything those frames hold is abandoned. Today they hold nothing counted, because `keel.trap` is
`@no_alloc` and everything it reaches is checked through every call — but `@no_alloc` forbids
*allocation*, not *holding a share*: a syscall handler that matched an `Option[&T]` field would take a
count and leak it at every `exit`, silently (finding 23's mechanism). The asm `extern` declarations are
the only statement of the contract, and neither says "returns twice" or "abandons its callers".

Workaround (used): the abandoned path holds only raw pointers and scalars, by care; `run` (which does
hold the process) is above the saved SP and returns normally.

Clean form: finding 23's — a way to say a call abandons frames, so the frames it abandons are checked
to hold no counts (or have them released); here, as an attribute on the `extern` that `leave_user`
is, propagated to every function that can reach it, the shape `@no_alloc` already has.

## 35. A runtime cannot call `main`, though on a freestanding image calling it is the entry's job (minor)

Milestone 6, `user/rt`. A user program's statements are compiled into `main`, and its entry `_start`
is the runtime's. The runtime's sysl half wanted to call `main` and pass its answer to `exit`:

    extern "main" program_main() -> int
    error: 'main' is where the platform starts this program, so an 'extern' may not name that symbol

On a target whose `entry` is `_start` the platform starts at `_start`, and packages.md says calling
`main` is the entry's job. Workaround (used): `_start` is `bl main; bl keel_user_finish` in assembly,
and `keel_user_finish(code) -> never` is the sysl half. Clean form: allow it when the manifest's
`entry` is not `main`.

## Milestone 7: virtio-blk

Findings 36-38 are against the same prerelease `1a7aee7358`. Found while writing `keel.virtio`, a
modern virtio-mmio block driver (a split virtqueue in two frames, eight requests in flight, completion
by an SPI that wakes the sleeping thread), and while making a kernel bounds failure fatal (keel now
exports `sysl_bounds_fail`). Ranked: 36 is what every DMA driver needs and is where a wrong answer is a
device writing to the wrong memory; 37 is silent and hangs the machine; 38 answers milestone 4's
question about `async` in drivers. None was fixed here.

## 36. A slice does not say where its storage is, so a driver cannot hand a caller's buffer to a device

Milestone 7, `read_sector(n, buf: []u8)`. A device reads and writes memory by *physical* address, in
runs that must be physically contiguous. The natural driver puts the caller's buffer straight into the
data descriptor:

    sh.desc[d + 1].addr = u64(mm.to_phys(mm.VirtAddr(u64(usize(as_mut_ptr(buf))))))

It compiles for every buffer, and it is right for only some of them. keel's memory has three kinds of
kernel address: the linear map (frames, heap blocks, module storage), where physical = virtual minus
`KERNEL_BASE`; the thread-stack window at `KERNEL_BASE + 0x40_0000_0000` (the stack-guard test faults
at `far=0xffff00400000bff8`), mapped page by page from whatever frames were free, so subtracting gives
an address past the end of RAM; and a heap block of 2 KiB or more, which is a run of frames — contiguous
— but a 512-byte block in a slab is contiguous only because a slab is one frame. The boot test's own
callers pass `b[..]` of a local `[512]u8`, which is on a thread stack: the device would have been
handed physical address `0x40_0000_xxxx`, which is not memory at all, and on real hardware would have
written there.

Nothing in the type of `buf` can say which kind it is, and nothing in sysl lets a function ask for
"memory a device can reach": no qualifier on a slice or pointer for its provenance, no way to state
"physically contiguous for `n` bytes", and no physical-address operation that could refuse where it does
not know. The DMA region itself has the same problem one level down: `mm.alloc_frames(2)` answers a
`Frame`, and turning it into a `*Shared` is `ptr_cast(usize(u64(mm.to_virt(pa))))` — nothing ties the
struct's size to the frame count but an `@assert(sizeof(Shared) == 0x2000)` written by hand, and
`phys(usize(&sh.headers[s]))` is correct only because `sh` came from the linear map, which no type
records.

Linux has the same split (`dma_map_single` refuses a vmalloc or stack address at run time, with
`CONFIG_DMA_API_DEBUG`); Rust's embedded HALs mark DMA buffers with a `'static` + `StableDeref` bound so
the buffer cannot move or die while the device holds it.

Workaround (used): the caller's bytes never reach the device. Each request slot owns a 512-byte bounce
buffer inside the queue's own frames, and `read_sector`/`write_sector` copy through it — one extra copy
per sector, and the physical address is only ever taken of memory the driver allocated from the frame
allocator.

Clean form: a pointer or slice qualifier for memory a device may reach (`*dma T`, `[]dma u8`), which
only an allocator of such memory produces and which carries, or can answer, its physical address;
an ordinary `[]u8` refusing to convert to it without a named, checked operation. Short of that, a
standard way for a kernel to state its own address-space regions so a physical-address function can
refuse an address outside the linear map.

## 37. Nothing marks a function that may sleep, so an interrupt handler that blocks compiles and hangs

Milestone 7, the disk's interrupt. `virtio.interrupt()` runs in IRQ context (from `keel_trap`, slot 5,
on the interrupted thread's stack). It must take completions and wake threads, and must never sleep:
sleeping there switches away from an exception frame, with the GIC still waiting for its EOI. Nothing
says so. With one line added to `irq.dispatch`:

    else if id == virtio.irq()
        virtio.interrupt()
        var probe: [512]u8 = [0; 512]
        val _ = virtio.read_sector(1, probe[..])      // blocks on a WaitQueue

the kernel builds with no diagnostic — `keel.trap`'s `@no_alloc` is checked through every call and is
satisfied, since a blocking read allocates nothing — and the boot test prints `keel: disk read ok` and
then nothing: `FAIL: QEMU did not exit within 8s`.

`@no_alloc` is the right shape (an attribute checked through every call) for the wrong property.
Linux's answer is `might_sleep()`, a run-time check; Zephyr asserts `!k_is_in_isr()` in every blocking
call. Both are conventions a reviewer has to know about.

Workaround (used): care, and a comment on `interrupt()` saying it runs in IRQ context.

Clean form: a function attribute in the `@no_alloc` family — `@no_block` on a module or function,
checked through every call, with the blocking primitives (`WaitQueue.sleep`, `Mutex.lock`, `block_on`)
marked as blocking at their declaration (`@blocks`), so an IRQ path that reaches one is refused naming
the chain.

## 38. `async` would not have improved this driver: the waker may not be called from the interrupt, and the reference's answer is the mechanism keel already has

Milestone 7, the question milestone 4 left open: driver completions as tasks inside a kernel thread.
Not built; what writing the driver showed, checked against `reference/async.md`:

- **The completion arrives in an interrupt, and a waker may not be called there.** The reference is
  explicit: a waker is a `&Fn() -> unit` belonging to the executor's domain, and the advice in its own
  refusal is *"let the other domain — an interrupt, another thread — signal the executor, and have the
  executor call the waker"*. Signalling the executor from the interrupt is exactly what
  `virtio.interrupt()` already does for a thread: set `done` under the IrqLock and `wake_one` the
  sleeper. A task-based driver keeps that handoff and adds a layer above it: the executor (a kernel
  thread) wakes, steps the task, the task reads the status.
- **The rule is not enforced where a kernel would break it.** The `&sync` capture check stops at module
  storage (finding 22), so a waker parked in a module `var` and called from an exported interrupt
  handler builds for `aarch64-freestanding` with no diagnostic:

      var on_complete: Option[&Fn() -> unit] = None
      arm(wake: &Fn() -> unit)
          on_complete = Some(wake)
      @export("disk_irq")
      disk_irq()
          on_complete match
              Some(wake) -> wake()
              None -> ()
      async read_sector(n: u64) -> u64
          await park(arm)
          n * 2

  (`sysl build-c --target aarch64-freestanding`: `wrote asyncprobe.a`.) That is the shape a kernel would
  write first, and it touches non-atomic counts from interrupt context.
- **Each `park` allocates** (its waker, and every task frame, through `sysl_task_alloc`), which the
  interrupt path could not do under `@no_alloc` even if it were allowed to call the waker.
- **What the blocking design costs is small.** Per request: a `done` flag, a `WaitQueue`, and a
  three-line loop (`while !done: waiters.sleep(&lock)`). The API is callable from any thread on any
  core, which is what a filesystem above it wants.

Where async *would* help is one kernel thread with many requests outstanding (readahead, a filesystem
that issues a run of sector reads and waits for all of them) — today that needs a thread per request or
a hand-written completion counter. For that, sysl would need an interrupt-safe waker: a waker that is a
`&sync` value (or a plain index into an executor's table, no count at all) whose `wake()` is
`@no_alloc` and may be called from another domain, so an interrupt handler can call it directly; and an
executor contract whose idle step can block the kernel thread (`WaitQueue.sleep`) rather than spin.

Workaround (used): none needed — the driver is kernel threads and wait queues, as decided.

Clean form: an interrupt-safe waker kind in the executor contract, as above; and finding 22's check
extended to module storage, so the probe above is refused.

## Milestone 8: a filesystem

Findings 39-42 are against the same prerelease `1a7aee7358`. Found while writing `keel.fs` (a buffer
cache, inodes, directories, twelve system calls), `keel.ondisk` (the on-disk structs, compiled into
both the kernel and a hosted tool), `tools/mkfs` (build, check and read an image on the host) and two
user programs over the new calls. Ranked: 39 is silent — bytes read off a disk become typed values
nothing checked, and the one helper every filesystem writes is a raw cast; 40 is milestone 9's whole
question, what `sysl.fs` needs from a target; 41 is a trap waiting at the system-call boundary; 42 is
minor. None was fixed here.

## 39. Bytes cannot be viewed as a struct, or a struct as bytes, except by a raw cast that checks nothing

Milestone 8, `keel.ondisk`. An on-disk format is structs laid in blocks: a superblock, 64-byte inodes,
32-byte directory entries. Both sides read and write them in place, through one helper both compile:

    at[T](bytes: []u8, offset: usize) -> *T
        if offset + sizeof(T) > bytes.len then panic("a struct past the end of its block")
        ptr_cast(usize(as_mut_ptr(bytes)) + offset)

and the other direction, for `fstat` and `getdents` copying a `Stat` or a `Dirent` out to a program, is
`val b: *u8 = ptr_cast(v); b[0..<sizeof(T)]`. Both are `ptr_cast`, the raw tier, and that is the only
way there is. The bounds check above is the helper's own; nothing else is checked, and the probe shows
what that leaves (hosted, `sysl run`):

    enum Kind: u16
        File = 1
        Dir = 2
    struct Node
        kind: Kind
        open: bool
        next: &Node
    var disk: [64]u8 = [0xff; 64]
    val n: *Node = at(disk[..], 1)
    print(n.open)                          // true — a bool whose byte is 0xff
    print(n.kind match                     // dir — 0xffff is neither variant, and the match took the
        File -> "file"                     //   last arm rather than trapping (finding 9's mechanism)
        Dir -> "dir")

It compiles with no diagnostic: a struct holding an enum, a `bool` and a counted reference — none of
which may hold arbitrary bytes — is "viewed" over a disk block, at offset 1, misaligned for every field.
A filesystem reading a corrupt disk has exactly these bytes. keel avoids it by care: `keel.ondisk`'s
structs are integers and integer arrays only, `Inode.kind` is a `u16` with constants rather than an
enum, and every offset is a multiple of the struct's size. Nothing in the types says any of that, and
nothing says the struct's byte order is the format's (both readers are little-endian; a struct cannot
state it — the milestone-7 note on wire structs).

Rust has the same shape as a library contract (`zerocopy`'s `FromBytes`/`AsBytes`, derived only for a
struct whose fields all accept any bit pattern and which has no padding); C has none and relies on care.

Workaround (used): integer-only structs, `@assert`ed sizes and offsets, the raw-cast helper above.

Clean form: a marker (a trait the compiler derives, or an attribute) for a plain-data struct — every
field an integer, an array of them or another such struct, no padding — and two checked library
operations over it: a view of `[]u8` as `*T` (or a copy into a `T`) that checks length and alignment,
and a `[]const u8` of a `*T`. Optionally a byte order on the struct or the field
(`@byte_order(little)`), honoured by those operations.

## 40. `sysl.fs` is bound to C's stdio and libc; a target with system calls and no libc has nothing to supply, and nothing says so until the link

**ANSWERED (2026-10-09, prerelease `ab820b7bf1`), in the clean form proposed below.** `sysl.fs` now
runs over `sysl.fs.sys`, twenty-five descriptor-level hooks (`sysl_fs_open` … `sysl_fs_realpath`, a
path as a pointer and a length, an `int` status that is an `IoError` code or `-1` for unsupported);
the hosted library supplies them over libc, and a freestanding program that reaches one without
supplying it is refused at compile time, naming exactly the hooks it reached. keel's `user/fsys`
supplies all twenty-five over its system calls (milestone 9). What is left of this finding is 43 (one
libc call left on the no-libc path) and 44 (what is still gated on `posix`).

Milestone 8, looking ahead to milestone 9 (a `cp` written with `sysl.fs`, running on keel). What the
module calls, from `library/sysl/fs/` (sysl-keel-pre2):

- **buffered stdio**: `fopen` (with C mode strings — the file's header explains the choice: `O_CREAT`
  and friends differ between platforms, mode strings do not), `fread`, `fwrite`, `fflush`, `fseek`,
  `ftell`, `ferror`, `feof`, `fclose`;
- **POSIX by path**: `access`, `unlink`, `rmdir`, `rename`, `mkdir`, `chmod`, `truncate`, `getcwd`,
  `chdir`, `symlink`, `link`, `readlink`; `opendir`/`closedir`;
- **C shims in `__posix__/`** (`stat.c`, `dirent.c`, `dirs.c`, `meta.c`, `errtext.c`): `fstat` +
  `fileno` for a file's size, `ftruncate`, a `stat` that copies a few fields out of `struct stat`, a
  `readdir` that hands back the name, `realpath`, a temporary directory;
- **errno** through `__error` (BSD) or else `__errno_location` (glibc), and `arc4random_buf`/
  `getentropy` for `publish`'s temporary names.

So a port has two shapes: give keel a small libc (stdio over `read`/`write`/`lseek`, a `struct stat`,
`opendir`/`readdir` over `getdents`, an errno) for `sysl.fs` to bind unchanged, or give `sysl.fs` a
platform layer below the stdio choice that a target can supply in sysl. Neither exists, and the module
does not say which is meant. A user program importing it, built for keel's target:

    import sysl.fs
    val text = fs.read_text("/hello.txt") match ...

    $ sysl build --target aarch64-freestanding -o probe.elf user/probe
    ld.lld: error: undefined symbol: __errno_location   >>> referenced by sysl.fs$why
    ld.lld: error: undefined symbol: fopen              >>> referenced by sysl.fs$opened
    ld.lld: error: undefined symbol: fclose ...

The module's header is `@requires(os)`, and the freestanding target is not refused at compile time —
the program compiles and fails at the link, naming glibc's errno accessor on a target that is not
Linux. (`reference/packages.md`: a capability the manifest does not mention is provided; keel's
programs mention none, so `os` counts as provided. That is the documented rule, and it is what lets a
freestanding image link `sysl.fs` far enough to fail there.)

What keel's calls give a port, as designed here: a path is a pointer and a length (a sysl `string`
crosses as it is, no NUL needed); `open` takes Linux's `O_*` values and errors are Linux's numbers, so
a libc's tables read them unchanged; `fstat`/`stat` fill a fixed 24-byte `Stat` (ino, kind, mode,
links, size — no times yet); `getdents` returns fixed 32-byte records. Missing for `sysl.fs` as it
stands: `access` (stat answers it), `chmod`, `truncate`/`ftruncate`, a current directory
(`getcwd`/`chdir`), links, timestamps (`Meta.modified`), and a source of randomness.

Workaround: none needed in this milestone; this is milestone 9's design input.

Clean form: a seam in `sysl.fs` at the level of a file descriptor (open/read/write/seek/close/stat/
directory listing/errno), written once over libc for hosted targets and suppliable by a target that
has system calls and no libc — a per-target directory like `__posix__`, or a trait the target's
runtime implements; and `@requires(os)` taken to mean "needs that seam", refused at compile time on a
target that does not declare it.

## 41. An integer that is not one of an enum's values traps on conversion, and there is no conversion that answers instead

**WITHDRAWN (2026-10-08): the premise is wrong.** sysl has the fallible conversion: `E.try(n)`
(`reference/errors.md`, lowered by `emit_enum.sysl`'s `gen_enum_try`) answers instead of trapping. The
user runtime should use `Errno.try(n)`; what follows is kept as the record of the probe.

Milestone 8, the system-call boundary. The kernel's errors are `abi.Errno`, an `enum Errno: i64` with
Linux's numbers (`ENOENT = 2`, …); the kernel answers `-i64(e)` and that direction is clean. The user
runtime turns the answer back:

    private answer(r: i64) -> Result[u64, Errno] = if r < 0 then Err(Errno(-r)) else Ok(u64(r))

`Errno(n)` is the checked conversion (`reference/types.md`: an integer that is not a value of the enum
traps). A kernel newer than the program — one more error number — makes every call that returns it a
crash in the program rather than an error it can report:

    enum Errno: i64
        ENOENT = 2
        EIO = 5
    val n: i64 = 99
    print(i64(Errno(n)))                   // exit 133, nothing printed

Nothing answers "is this a value of the enum?": no `Errno.from(n) -> Option[Errno]`, no
`TryFrom`. A program can only write the reverse table by hand (`n match 2 -> Some(ENOENT) ...`), which
is the enum written twice. The same holds for any enum read from outside the program: a device
register, a disk (finding 39), a network packet.

Workaround (used): none — keel's kernel and runtime are built together, so every number the kernel
answers is a variant. A program built against an older `abi` would trap.

Clean form: a total conversion beside the checked one, compiler-supplied for every simple enum
(`Errno.of(n) -> Option[Errno]`, or `n as? Errno`), and optionally a way to declare an enum open
(`@open enum Errno: i64` keeping an `Other(i64)`-like reading for unknown values).

## 42. A function in a program's entry file may not be generic once anything around it captures (minor)

Milestone 8, `user/files`. A program's entry file is a body (`reference/modules.md § The entry file
is a body`), so its functions are nested functions of it. A helper that checks any call's error:

    var failures = 0

    failed[T](r: Result[T, int])
        if !r.is_ok() then failures += 1

    val r: Result[string, int] = Err(1)
    failed(r)

    error: 'failed' is declared inside a function body and cannot be generic — the type arguments
    would have nowhere to come from, since nothing outside the body calls it
    error: undefined name 'failures'

The reason given is not so: the body calls it, with a concrete type. The same function without the
capture (no `failures`) compiles and runs generic. The second diagnostic is a cascade (`failures` is
declared, above). Workaround (used): the helper takes `r.err()`, an `Option[Errno]`, instead of the
`Result`. Clean form: a nested function may be generic, instantiated at the calls in its body; or the
message names the capture as the reason.

## Milestone 9: sysl targets keel

Findings 43-46 are against the prerelease `ab820b7bf1`, the first with `sysl.fs.sys`. Found while
writing `user/fsys` (the twenty-five hooks over keel's system calls), five programs written with
`sysl.fs` (`cp`, `cmp`, `cat`, `ls`, `fscheck`), argv on the user stack and the calls `sysl.fs` needed
that keel lacked (`getcwd`/`chdir`, `truncate`/`ftruncate`, `access`). Ranked: 43 is a defect that
stops every `sysl.fs` program from linking on keel — the milestone's yardstick waits on it; 44 is what
`sysl.fs` still withholds from a target that can answer it; 45 and 46 are minor. None was fixed here,
and nothing in keel works around 43: the boot test is red against `ab820b7bf1` and green against a
scratch copy of its library with the one change below.

## 43. `sysl.fs`'s path for a target with no C library calls C's `snprintf`, and nothing says so until the link

**FIXED (sysl branch `fs-no-snprintf`, census-35 87ec6b74; prerelease `87ec6b7437`):** integer format
specs are rendered by sysl (`sysl.fmt.format_int`) on every target, and a reached float spec or `%s`
on a target with no C library is refused at compile time. Milestone 9's boot test passes unpatched.

**A defect, not a gap.** `sysl.fs.sys` is built so a target with system calls and no libc gets all of
`sysl.fs` by answering its hooks, and the compiler checks exactly that — a program missing a hook is
refused, naming it. `cp` supplies all of them and still does not link:

    $ sysl build --target aarch64-freestanding -o scratch/user/cp.elf user/cp
    sysl: error: clang failed (exit 1):
    ld.lld: error: undefined symbol: snprintf
    >>> referenced by sysl.code.o.ll
    >>>               sysl.code.o:(sysl.fs$IoError.message) in archive …/std.syslib

`IoError.message` (`library/sysl/fs/error.sysl`) formats with a counted spec in the two arms a no-libc
target reaches — `InvalidUtf8(at) -> f"not UTF-8 at byte $at%d"` and the non-`posix`
`system_text`, whose whole purpose is that target: `f"error $code%d"`. **A counted spec lowers to
`sysl.str.fmt_i`, which calls `snprintf`, on every target**; a plain hole does not. The shortest input
(`sysl build-c --target aarch64-freestanding`, then `llvm-nm -u` on the archive):

    @export("probe_counted")
    counted(c: int) -> usize = f"error $c%d".bytes.len     // U snprintf  (sysl.str.fmt_i)

    @export("probe_plain")
    plain(c: int) -> usize = f"error $c".bytes.len         // sysl.str.int — no C at all

Any program that prints an `IoError` — every `cp` that reports why it failed — reaches it.

Workaround: none taken. Defining `snprintf` in keel's runtime would be writing C's formatter in sysl
to cover a library bug. The fix is in `error.sysl`: `$code` and `$at` with no `%d` (the two values are
integers, so the spec adds nothing); with that change in a scratch copy of the library (`SYSL_LIB`),
the whole milestone passes. The second half is the gap under it: a counted spec needing libc is
invisible on a hosted target and is only found at a freestanding link, so either `fmt_i` stops
needing `snprintf` for the integer specs, or a freestanding build refuses a counted spec where it is
written, the way it refuses an unanswered hook.

## 44. `metadata`, the walk, `remove_dir_all`, `copy_dir_all`, `make_temp_dir`, `canonicalize` and `publish_dir` are `#if posix` though they are written over the hooks

A keel program that asks for a file's metadata gets an undefined name:

    import sysl.fs
    fs.set_permissions("/hello.txt", 0o600)      // or fs.metadata(p), fs.walk(root), fs.remove_dir_all(d)

    error: undefined function 'sysl.fs.set_permissions'

Each is written over `sysl.fs.sys` alone — `metadata`/`link_metadata` are one `fsys.stat` into
`Meta`, `set_permissions` one `fsys.chmod`, `make_temp_dir` one `fsys.temp_dir`, the walk and
`remove_dir_all` `entries` plus `link_metadata` — and keel answers every hook they reach: its `stat`
fills `mode` with POSIX's type bits, so `Meta.is_dir()` would read right. The gate's stated reason
(`meta.sysl`: "a Windows reading of a filesystem entry is a different struct") is about a C library's
`struct stat`, which the hook layer exists to hide: `sysl.fs.Stat` *is* the POSIX shape whoever
supplies it. So the gate is keyed on the wrong fact — whether the target has a POSIX C library —
rather than on whether something answers `sysl_fs_stat`. Milestone 10's `ls -l` and `rm -r` are
these calls.

Workaround: none needed in this milestone (`cp`, `ls`, `cat` use none of them). keel supplies
`chmod` and `temp_dir` anyway (answering `-1`), though nothing a keel program can call reaches them.

Clean form: the reading surface declared wherever `sysl.fs.sys` is (a non-POSIX supplier fills the
same thirteen numbers, or leaves them zero, as the struct's comment already allows), with the
Windows case answered by that platform's supplier rather than by a `#if` in the portable layer.

## 45. A runtime's exports vanish unless a file imports its module, while the module's top-of-file `asm` stays (minor)

keel's user runtime is a workspace member (`rt`) that carries `_start` (top-of-file `asm`: `bl main`,
`bl keel_user_finish`), the exported `putchar`/`exit`, and the allocating pair named by the program's
`allocator` block. `cp` names nothing from it — `print` and `exit` reach it by symbol — so its first
draft did not import it:

    // user/cp/main.sysl, with rt in the manifest's dependencies
    import sysl.fs
    main(args: []string) …

    ld.lld: error: …/user.ld:11: symbol not found: keel_user_alloc
    ld.lld: error: undefined symbol: keel_user_finish

The `asm` was laid down (it is never pruned) and calls `keel_user_finish`, but the module was not
reached, so its `@export`s were not roots and were dropped — the one half of the module needing the
other. The hooks module (`fsys`) is not imported by `cp` either and its exports *are* kept, because
they define `sysl.fs.sys`'s externs, which `sysl.fs` reaches. Workaround (used): every program writes
`import rt.keel.user`, unused. Clean form: a module's `@export`s are roots whenever its top-of-file
`asm` is laid down (or a manifest dependency that supplies the entry or the allocating pair counts
as reached).

## 46. On a target with no C library, an error `IoError` has no name for says only "error N", and the supplier cannot give it words (minor)

keel's errors that `IoError` does not name arrive as `Other(n)`, and its `message` on a non-`posix`
target is `f"error $code"` — so removing a file another process has open (keel answers `EBUSY`) says
`error 16`. On a hosted target the words come from the platform (`sysl_fs_strerror` over
`strerror_r`); that shim is the one call not in `sysl.fs.sys`, so a supplier that answers every
filesystem call cannot say what its own error numbers mean. Workaround: none needed (keel's `cp` never
meets one in the tests). Clean form: `sysl_fs_strerror` becomes the twenty-sixth hook, optional —
answered `-1` where the supplier has no words, which is today's behaviour.

## Milestone 10: processes and a shell

Findings 47-49 are against the prerelease `87ec6b7437`. Found while moving the user programs out of
the kernel's image onto the disk, giving keel a process model (`spawn`, `wait`, `getppid`, a process
table, orphans handed to init), an interrupt-driven console with a line discipline, and writing `init`,
`sh` and five more utilities in sysl. Ranked: 47 is a check that cannot be stated at the grain a
kernel needs, so the only way to keep it is to hide an edge from it; 48 is the standard library
reaching C's names for reading standard input, found only at the link; 49 is why the shell does not
use `sysl.process`. None was fixed here.

## 47. `@no_alloc` is a property of a whole module, so an exception entry whose interrupt path must not allocate and whose system-call path must cannot say so

`keel.trap` is `@no_alloc`: its IRQ path (`dispatch`, the timer, the scheduler's preemption, the
drivers' interrupt handlers) runs on whatever stack the interrupt landed on, and the check through
every call is what keeps it allocator-free. The same module's `keel_trap` also takes the synchronous
exception from EL0 — a system call — and hands it to `keel.proc.user_sync`. Milestone 10's `spawn`
has to allocate: the executable read into a buffer, the argument strings, the `Process` box, the
thread's closure. The build stops at the call in the trap module:

    error: this reaches 'keel.proc.lay_out_args', which makes heap storage, and this module declared
    '@no_alloc' — an allocator-free module may only call what is allocator-free itself
       --> kernel/keel/trap/trap.sysl:187:14
        |
    187 |         proc.user_sync(&frame.x[0], u64(syndrome(raw).ec), u64(syndrome(raw).iss))
        |              ^^^^^^^^^

The check is right that the call allocates, and wrong about whose promise it breaks: the system call
runs on the process's own kernel stack with interrupts enabled, where a kernel allocates as a matter
of course. The property belongs to the *IRQ entry*, and there is no way to put it there:

    @no_alloc
    @export("on_irq")
    on_irq(n: u64) -> u64 = n + 1

    error: this attribute belongs in the file's header, on the lines directly after 'module' and
    before everything else — it is a property of the whole module, not of the statements below it

The reference says the same (`modules.md`: "no execution that begins in this module's own code makes
heap storage"). The other arrangements both lose something: taking `@no_alloc` off `keel.trap` stops
checking the fault paths and the vector code's own calls; moving the dispatch of EL0's synchronous
exceptions out of the module needs a second vector entry in assembly, since every slot shares one
tail that calls `keel_trap`.

(While finding this, `echo([LF])` — a one-element array literal passed where a `[]const u8` is
wanted, in the console's line discipline — was reported as making heap storage too. It is the same
check working, but a constant one-byte argument is not where a kernel author expects an allocation.)

Workaround (used): `keel.trap` declares `extern "keel_user_sync" user_sync(...)` and `keel.proc`
`@export`s the function under that symbol. The check stops at an `extern`, so the one edge it should
not follow is hidden from it — and so would be any edge written that way, which is the cost: nothing
distinguishes "this edge allocates on purpose" from "this edge was hidden".

Clean form: `@no_alloc` on a function (an entry point), checked through every call from it, beside
the module form; or an annotation on a call or an `extern` saying the callee is outside the promise
(`@allocates`), so the exception is written where it is made and is visible to a reader.

## 48. `sysl.io`'s standard input and `sysl.flush` reach C's `read`, `memchr` and `fflush` by name, and a target with no C library learns so at the link

The shell reads its commands with `sysl.io.lines(&stdin())`. On `aarch64-freestanding` the program
builds and the link fails:

    ld.lld: error: undefined symbol: memchr
    >>> referenced by sysl.code.o.ll
    >>>               sysl.code.o:(sysl.io$Lines.line_end) in archive …/std.syslib

The shortest input (`sysl build-c --target aarch64-freestanding`, no diagnostic, then `llvm-nm -u`):

    import sysl.io.{lines, stdin}

    @export("probe")
    probe() -> usize
        var input = stdin()
        var n: usize = 0
        for line in lines(&input)
            n += line.bytes.len
        flush()
        n

    U exit  U fflush  U free  U malloc  U memchr  U putchar  U read

`malloc`/`free` are the manifest's allocating pair and `putchar`/`exit` are what `print` and `panic`
are documented to need from a freestanding program. The other three are not documented anywhere a
freestanding author would look: `FdReader.read` calls `sysl.sys.sysl_read` = C's `read`,
`Lines.line_end` calls `sysl_memchr` = C's `memchr`, and `flush()` is `fflush(NULL)`. `sysl.fs` was
given exactly the seam this lacks (finding 40): a freestanding program that reaches a filesystem
hook without supplying it is refused at compile time, naming the hook. Reading a descriptor is the
same operation as `sysl.fs.sys`'s `sysl_fs_read`, which keel already answers.

Workaround (used): keel's user runtime defines `read` (over the `read` system call; for descriptor
0 it first pushes out what `print` has collected, so a prompt appears before the shell waits),
`fflush` (pushes the same line out) and `memchr` (a loop) under C's names — as it already defines
`putchar`, `memcpy` and the rest.

Clean form: on a target with no C library, `sysl.io`'s descriptor reads go through a hook (or
`sysl_fs_read` itself) and `flush` through another, both covered by the compile-time check that
names what is missing; and `memchr` written in sysl there, as `putbytes` already is (`#if hosted`).

## 49. `sysl.process` is fork/exec over C shims, so a target with its own process calls has nothing to supply, and nothing in it can give a child a file as its output

The shell is the program `sysl.process` is for — start a program with a list of arguments, wait,
read how it ended — and it cannot use it on keel:

    import rt.keel.user
    import sysl.process

    process.run("/bin/ls", ["/"]) match
        Ok(_) -> exit(0)
        Err(_) -> exit(1)

    $ sysl build --target aarch64-freestanding … user/procprobe
    ld.lld: error: undefined symbol: sysl_proc_start
    ld.lld: error: undefined symbol: sysl_proc_wait

The module is `@requires(posix)`, and a manifest that mentions no capabilities provides them all
(`packages.md`), so a freestanding program reaches it and fails at the link, naming C shims in
`__posix__/spawn.c` (`fork`, `execvp`, `waitpid`) — the shape `sysl.fs` had before finding 40 was
answered. There is no hook layer to supply, though keel's `spawn`/`wait` are each one call. And the
surface would not carry a shell even with one: a child's output can be inherited or captured into a
buffer (`capture`, `start(…, stderr)`), but not sent to a file or a descriptor the caller opened, so
`>` has no spelling; and `Status` (exit code, signal, timed out) has no case for a child the kernel
ended for a fault, which is how keel reports one.

Workaround (used): the shell uses keel's runtime directly — `user.spawn(path, args, out_fd)`,
`user.wait(pid) -> (pid, ExitStatus)` — and `user.open` for the `>` file. Everything else it does
(the current directory, `cd`, `pwd`) is `sysl.fs`.

Clean form: a `sysl.process.sys` seam like `sysl.fs.sys` — start (program, argv, directory, and a
descriptor or "inherit" for each standard stream) and wait (pid → how it ended) — answered by
`__posix__` on a hosted target and by the program on one without a C library, under the same
compile-time check; a `Stdio`-like choice per stream (inherit, a file, a pipe, captured) in `run` and
`start`; and a `Status` case for "ended by the system" that is not a signal number.

## What worked in milestone 10

- **Spawn, not fork, cost almost nothing new.** `spawn` is `start` with a parent: the loader, the
  argument layout and the thread were milestone 6's, and the table is a fixed array of raw pointers
  under one `IrqLock`, each entry keeping its own box alive through `Process.me: Option[&sync
  Process]` exactly as a thread does. A parent's `wait` sleeps on one `WaitQueue` that every ending
  wakes; an ending hands its children to init under the same lock. `getppid` is one field.
- **Module storage with no initializer is zero, and zero is a valid lock, queue and table.** The
  console's `IrqLock`, `WaitQueue`, `Mutex[Line]` and ring, and the process table, are declared with
  no initializer; `emit-llvm` shows no `@main` (finding 31's trap was checked for, not hit).
- **An exit status is `exit(code)`, and the compiler says so.** `main() -> int` is refused: *"'main'
  yields nothing or a 'Result[unit, E]', so it may not result in int — a program's exit status is not
  something a signature can say. 'exit(code)' is how a program chooses its own"*. `false` is
  `exit(1)`; the runtime's `exit` already ends the process with the code.
- **The shell's string handling needed nothing missing**: words collected as `Buf[u8]` and turned into
  `string`s with `from_utf8`, quotes handled byte by byte, `==` on strings, `s"/bin/$name"`. 137
  lines with its comments; 29.9 KiB of text.
- **`sysl.io.lines` over the console is the whole of the shell's input loop** once the runtime answers
  `read` (finding 48): the kernel hands out one cooked line per `read`, so the reader never holds
  bytes of the next command.
- **Size**: the kernel image is 245 KB, from 1.41 MB with every program embedded; the disk carries 22
  programs in 648 of its 1,024 blocks, most of each ELF being the 64 KiB segment alignment.
- **Timing**: per boot 1.1-1.4 s under HVF, 1.7-1.8 s at -O0, 3.4 s under TCG (the programs are read
  from the disk now, and each boot runs a shell session).
- Controls: `wait` answering code 0 for every child fails the transcript (`$ false` and no `sh:
  false: exit status 1`); a console that echoes only the newline fails it at the first command (`$ `
  then `hello`).

## What worked in milestone 9

- **The hook layer is the right seam.** Twenty-five `@export`s in a workspace member of their own
  (`user/fsys`, 227 lines with comments), each a line or two over a system call, and `copy_file`,
  `read_bytes`, `entries`, `File` with its buffering, `truncate`, `set_current_dir` and the error
  values all worked on the first boot. The compile-time check named exactly the five hooks `cp`
  reached (`open`, `read`, `write`, `seek`, `close`), and a workspace library — not the program —
  was accepted as the supplier.
- **One mapping, `Errno.try`.** `status(r)` turns a negative answer into a code: `Errno.try(-r)`, a
  `match` over the variants `IoError` names, `ENOSYS` as unsupported, anything else `Other`; an
  unknown number is `Other` too rather than a trap (finding 41's withdrawal confirmed in use).
- **argv needs nothing from sysl but `_start`.** On `aarch64-freestanding`, `main(args: []string)`
  is laid out as C's `main(i32 argc, ptr argv)` calling `sysl.args.args_of`, exactly as on a host; the
  kernel writes Linux's layout at the top of the stack and `_start` loads `x0 = [sp]`, `x1 = sp + 8`.
  `echo` proves it; 600 arguments are refused at `start_with` (`TooManyArgs`).
- **Exit status** is `exit(n)`, the platform's `extern` the runtime already exports; `cp` exits 1 on
  a failed copy and 2 on a bad command line. (`main -> Result[unit, E]` gives only 1.)
- **Size**: `cp` is 16.8 KiB of text (hello: 3.3 KiB); the largest pieces are `File.read` 1.4 KiB,
  `copy_file` 1.3 KiB, `IoError.message` 1.1 KiB. The ELF is 97 KB, nearly all segment alignment
  (`user.ld`), not code.
- Controls: a `read` hook that reports one byte fewer than it delivered fails the guest's `cmp`
  (`cmp: EOF on /m9/copy.txt after byte 17`, `/m9/big.bin /m9/big.copy differ: byte 8192`), four
  `fscheck` checks, and the host's `cmp` of `mkfs get /m9/big.copy` (25 bytes short, `differ: char
  8192`).

## What worked in milestone 8

- **One source, two targets, one workspace.** `ondisk/` is a workspace member with no target of its
  own; the kernel (`aarch64-freestanding`, strict-align, no FP) and `tools/mkfs` (the host) both depend
  on it with `{ workspace = true }`, and `sysl build tools/mkfs` and the kernel build each compile it for
  their own machine. `@assert(sizeof/offsetof)` pins the layout once for both.
- **`Result[T, Errno]` with `?` through the whole filesystem**, and one conversion to the register at
  the dispatch (`Err(e) -> u64(-i64(e))`). A simple enum with explicit values is exactly an errno
  table; matching on one variant (`Err(ENOENT) ->` to create a missing file) reads as intended.
- **`defer unlock()` with early `return`s and `?`s** in every call that takes the filesystem lock.
- **The whole filesystem state inside a `Mutex[State]` in a zero-initialised module `var`**: no
  initializer, no `@main` entry (finding 31 checked with `emit-llvm`), and the only way to the state is
  through the lock.
- **Paths as `[]const u8`** in the kernel: no allocation, no UTF-8 question, and the user side passes a
  `string`'s bytes and length as they are. `string` and `s"..."` on the kernel heap were used only for
  messages.
- **The hosted tool was ordinary sysl**: `sysl.fs.entries`/`read_bytes`/`write_bytes`, `Buf`, `split`.
- **A user program's `[0; 1000]` local needs `memset`**: the user runtime now carries keel.rt's four
  functions too (finding 18's workaround, a second copy).
- Controls: `WRITE_BACK = false` in `keel/fs/cache.sysl` (sync writes nothing) passes boot 1 and fsck,
  and boot 2 prints `user: first boot, no /out.txt yet` instead of `user: persisted across reboot`;
  `keel: fs synced at shutdown, 0 blocks written`. A bitmap bit cleared under an owned block gives
  `host: fsck: block 7 is owned by inode 1 and marked free`; one set for a free block gives `host: fsck:
  block 500 is marked used and nothing owns it`; both exit 1.

## What worked in milestone 7

- **`volatile` struct fields for a ring the device writes.** `Desc`, `Avail`, `Used` and `UsedElem` are
  plain structs with `volatile` fields; `sh.used.ring[i].id` through a `*Shared` is a single 32-bit load
  each time, and `[SLOTS][SECTOR_SIZE]u8` nests. `@assert(offsetof(...))` pins the virtio-mmio
  register block and the queue layout against the specification, as it did the GIC's.
- **The barrier built-ins are exactly the vocabulary virtio needs**: `dmb(ishst)` before publishing the
  available index, `dmb(oshst)` between it and the notify store to Device memory (Linux's `writel`
  barrier), `dmb(ishld)` between reading the used index and its elements, `dsb(st)` before
  `QueueReady`. No asm block in the driver. (finding 28's items 1 and 2 are answered by them.)
- **Module storage with no initializer is zero, and `@main` stays empty**: `private var disk: Disk` (an
  `IrqLock`, a `WaitQueue`, pointers, counters) and `private var slots: [8]Slot` fill no `main`
  (finding 31 checked with `emit-llvm`).
- **`sysl_bounds_fail` is an `@export` away.** keel's version (in `keel.trap`, `@no_alloc`) prints the
  form, values, length and `file:line:column` — `keel: bounds check failed: index a=121 b=0 length=3
  at keel/selftest/bounds.sysl:15:15` — and powers off. The location string is NUL-terminated, read
  through a `*u8`.
- **Wire structs at their natural order.** virtio's fields are little-endian, which is aarch64's order,
  so the structs are plain integers; a struct field cannot state a byte order (`sysl.encoding`'s
  `get_u32_le` works on byte slices, not on a volatile field), which would matter only on a big-endian
  target, of which sysl has none.
- **Unsigned arithmetic wraps**, so the free-running 16-bit ring indices are `i + 1` on a `u16`.
- Padding the queue's struct to place each ring on its own boundary took hand-counted `pad` arrays
  (finding 30: a field cannot ask for alignment).
- Controls: not waiting for the used ring (`run` returns after the notify) fails at once — `disk read
  failed: device error`, `disk smp wrong: good 64 bad 336`. Not acknowledging the interrupt either hangs
  the run (an interrupt storm on the boot core) or, when the other cores finish first, shows as 719
  interrupts for 400 requests; the test now fails on more interrupts than requests. **Removing all three
  ring barriers does not fail** (5/5 HVF, 3/3 TCG): QEMU's device model reads the rings in its own
  thread after a trapping MMIO notify, which serialises the guest's stores, so no test here can show a
  missing virtio barrier; they are right by the specification. Without keel's `sysl_bounds_fail`, the
  compiler's `brk #1` now halts through the trap handler (`not keel's breakpoint, halted`), and the
  ordered check fails on the missing `bounds check failed` line.

## What worked in milestone 6

- **A user program is ordinary sysl.** `print`, `s"..."` interpolation, `val`/`var`, `loop`, a
  `for` over a range, `*volatile u64` — all at EL0, built by `sysl build --target
  aarch64-freestanding` with the program's own `targets` block naming `../user.ld` (linked at 4 MiB).
  The runtime it needs is one 150-line module, `user/rt`: `_start` (three instructions), four `svc`
  wrappers (an `asm` block each, `in nr : "x8"` / `out r : "x0"`), `putchar` line-buffered over
  `write`, `exit` for `panic`, and a first-fit allocator over `brk`. `hello` is 2.5 KB of text.
- **A workspace holds the kernel and the programs.** `workspace = ["kernel", "user/*"]`; `user/abi`
  (the call numbers and errno values) is a member the kernel and the runtime both depend on with
  `abi { workspace = true }`, so the two sides cannot disagree about a number; `user/rt`'s manifest
  declares the `allocator` once for every program (with link.ld aliases, finding 19). Each program
  states its own `codegen` and `targets` — only the root project's apply — which is right.
- **The ELF loader's structs are sysl structs.** `ElfHeader` and `ProgramHeader` with `offsetof`
  `@assert`s against the specification, read by a generic `read_at[T](bytes, at) -> Option[T]` that
  copies into a zero-initialized `var out: T` byte by byte — no alignment assumption under
  `strict_align`, the byte order aarch64's and ELF's alike. (`sysl.encoding.get_u32_le` is there for a
  format of the other order.)
- **`@no_alloc` held across the new trap path.** System calls, user faults and the kill check are
  reached from `keel_trap`, and the module-level check went through all of `keel.proc`'s handlers and
  the page-table code without a change.
- **`*extern(A) -> R` spells a handler table**; only finding 31 stopped it.
- **A `new` type for a user address** refuses `*a` and `ptr_cast(a)`, which is most of finding 32's
  value already.
- Controls: `TTBR0_SWITCH = false` in sched.sysl (TTBR0 loaded once, when a process starts) breaks the
  preemption test — a spinner resumed on another core finds another process's pid in its own memory
  (`keel: process 5 exited 3`), another takes a translation fault (`process 6 killed`), and `preempt
  user wrong: spinners killed 2 of 4`. `VALIDATE_USER_POINTERS = false` is finding 32's control.

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
