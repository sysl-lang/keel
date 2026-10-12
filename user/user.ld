/* A keel user program's image. keel loads its PT_LOAD segments into a fresh address space (TTBR0,
   the low half) at the addresses they were linked at, so this is all a program's layout is: text from
   4 MiB, read-only data on the next page, then data and .bss. The kernel puts the heap after the last
   segment and the stack high above it (keel.proc), and leaves page 0 and everything below 4 MiB
   unmapped so a null pointer faults. */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);

    /* The template of a thread's `@thread_local` storage (the PT_TLS segment): `.tdata`'s initial
       values, then `.tbss`'s size. Nothing reads it in place — keel.user's start and sysl.threads copy
       it into each thread's block — so it opens the writable segment rather than sharing a page with
       `.rodata`. */
    .tdata : {
        *(.tdata .tdata.*)
    }

    .tbss : {
        *(.tbss .tbss.*)
    }

    .data : {
        *(.got .got.plt)
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
