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
