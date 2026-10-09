/* keel's image, for QEMU `virt`: RAM begins at 0x4000_0000 and QEMU puts the device tree at its
   start, so the image is loaded 2 MiB in. The tree QEMU builds is 1 MiB, and QEMU loads it only if it
   fits below the image: with the image half a megabyte in, it silently loaded no tree at all.

   It is LINKED in the high half, at KERNEL_BASE plus that physical address (keel.mm has the map), and
   LOADED at the physical address: each section's AT() is its address minus KERNEL_BASE. QEMU loads
   by the physical addresses and, since the entry point lies in an executable segment whose virtual
   and physical addresses differ, starts at `_start`'s physical address.

   The boundaries keel.mm maps with different permissions are page aligned: text (read-only,
   executable), read-only data and the GOT, then data, .bss, an unmapped guard page and the stack
   (read-write, never executable). */

ENTRY(_start)

/* The allocating pair package.hocon names, as aliases of keel.heap's exported functions
   (FINDINGS.md 19). */
keel_heap_alloc = keel_alloc;
keel_heap_free  = keel_free;

/* The ownership runtime's per-thread drain list. The runtime carries a weak `__sysl_arc_reaper` in the
   program's own module, so a sysl function exported under that name is a second definition clang
   refuses (FINDINGS.md 21); keel.sched exports `keel_arc_reaper` and this assignment wins over the
   weak one. */
__sysl_arc_reaper = keel_arc_reaper;

KERNEL_BASE = 0xFFFF000000000000;   /* keel.mm.KERNEL_BASE */
LOAD_ADDR   = 0x40200000;

SECTIONS
{
    . = KERNEL_BASE + LOAD_ADDR;
    __image_start = .;

    .text : AT(ADDR(.text) - KERNEL_BASE) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    __text_end = .;

    .rodata : AT(ADDR(.rodata) - KERNEL_BASE) {
        *(.rodata .rodata.*)
    }

    .got : AT(ADDR(.got) - KERNEL_BASE) {
        *(.got .got.plt)
    }

    . = ALIGN(4096);
    __rodata_end = .;

    .data : AT(ADDR(.data) - KERNEL_BASE) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __stack_guard = .;
    . += 4096;

    .stack (NOLOAD) : {
        . += 0x10000;
        __stack_top = .;
    }

    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

ASSERT(__image_end <= KERNEL_BASE + 0x40400000,
       "keel's image has to end inside the second 2 MiB of RAM, which keel.mm maps page by page")
