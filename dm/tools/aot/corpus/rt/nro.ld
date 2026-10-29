/* The corpus programs' layout (dm/tools/aot/corpus): an NRO as libnx's switch.ld lays one out, reduced to what a program without libc needs.
   Three loadable segments at page-aligned addresses from 0 (text, read-only data, data with bss), which tests/elf2nro.py turns into the NRO's
   segments; the NRO header overwrites text bytes 0x10..0x80, which crt0.S leaves free. GNU ld (devkitA64) and ld.lld read it alike. */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    code   PT_LOAD FLAGS(5);   /* read, execute */
    rodata PT_LOAD FLAGS(4);   /* read */
    data   PT_LOAD FLAGS(6);   /* read, write */
    tls    PT_TLS;
    dyn    PT_DYNAMIC;
}

SECTIONS
{
    . = 0;
    .text :
    {
        KEEP(*(.crt0))
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .text.*)
        . = ALIGN(8);
    } :code

    . = ALIGN(0x1000);
    .dynsym   : { *(.dynsym) } :rodata
    .dynstr   : { *(.dynstr) } :rodata
    .hash     : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.*) } :rodata
    .relr.dyn : { *(.relr.*) } :rodata
    .rodata   : { *(.rodata .rodata.*) . = ALIGN(8); } :rodata
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata
    .eh_frame_hdr : { *(.eh_frame_hdr) } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata
    PROVIDE_HIDDEN(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
    PROVIDE_HIDDEN(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
    .note.gnu.build-id : { *(.note.gnu.build-id) } :rodata

    . = ALIGN(0x1000);
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) . = ALIGN(8); } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got     : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    .data    : { *(.data .data.*) . = ALIGN(16); } :data
    .tdata   : { *(.tdata .tdata.*) } :data :tls
    .tbss    : { *(.tbss .tbss.*) } :data :tls
    .bss     :
    {
        PROVIDE_HIDDEN(__bss_start__ = .);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        PROVIDE_HIDDEN(__bss_end__ = .);
    } :data
    ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) <= 240, "corpus TLS: zero-initialised only, 240 bytes at most (rt.c, rt_tls_init)")

    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.interp) }
}
