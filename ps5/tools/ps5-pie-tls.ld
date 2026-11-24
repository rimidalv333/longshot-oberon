/*
 * (base PS5) PS5_Vulkan's tooling/native/ps5-pie.ld + tooling/psbc/ps5-pie-unwind.ld, for a title built with native
 * thread-local storage (ps5/build.py PS5_NATIVE_TLS, the default; link.sh uses this script then, PS5_Vulkan's otherwise).
 *
 * Two changes, and nothing else moved:
 * - a PT_TLS header: lld refuses STT_TLS symbols without one ("has an STT_TLS symbol but doesn't have a PT_TLS segment").
 *   ps5-native-tool link (sce_module_writer.cpp) rebuilds the eboot's own PT_TLS from the SHF_TLS sections: their extent and
 *   max(0x20, alignment). The TLS sections are aligned to 32 here, so lld's p_align (with which it computed every
 *   local-exec offset, -alignTo(p_memsz, p_align) + offset) and the converter's are the same, as is p_memsz. Not more:
 *   the console's loader refuses a PT_TLS aligned past 32 ("rtld: scan_phdr: alignment of segment 7 is 40. it must be
 *   less than 32", the first native-TLS eboot). No object of the title asks more than 32 of its TLS sections.
 * - .tdata and .tbss before .data, not after it: the converter stores the data segment's bytes up to the end of its last
 *   non-NOBITS writable section other than TLS ones (data_stored_end). After .data, .tdata's initial values were past it,
 *   loaded as zeros. Before .data they are inside it. (.tbss takes no address space for the sections after it.)
 * host/ps5/tls_check.c checks at boot that a second thread gets its own, initialized copy.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    text PT_LOAD FLAGS (0x5);
    ro PT_LOAD FLAGS (0x4);
    relro PT_LOAD FLAGS (0x6);
    data PT_LOAD FLAGS (0x6);
    dynamic PT_DYNAMIC FLAGS (0x6);
    tls PT_TLS FLAGS (0x4);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text .text.*)
        *(.plt .plt.*)
    } : text

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) {
        KEEP(*(.eh_frame_hdr))
    } : ro
    .eh_frame : { KEEP(*(.eh_frame)) } : ro
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } : ro
    .rodata : { *(.rodata .rodata.*) } : ro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } : relro
    .got : { *(.got) } : relro
    .got.plt : { *(.got.plt) } : relro
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } : relro
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } : relro
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } : relro

    /* Kept only long enough for the converter to read and rewrite them. */
    .dynsym : { *(.dynsym) } : relro
    .gnu.hash : { *(.gnu.hash) } : relro
    .dynstr : { *(.dynstr) } : relro
    .rela.dyn : { *(.rela.dyn) } : relro
    .rela.plt : { *(.rela.plt) } : relro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } : data : dynamic
    .tdata : ALIGN(32) { *(.tdata .tdata.* .gnu.linkonce.td.*) } : data : tls
    .tbss : ALIGN(32) { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } : data : tls
    .data : { *(.data .data.*) } : data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } : data
}

PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
