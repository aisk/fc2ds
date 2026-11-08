// Data extracted from the user's ROM at build time (see Makefile).

    .section .rodata
    .balign 4

    .global @NAME@_chr
@NAME@_chr:
    .incbin "chr.bin"
    .global @NAME@_chr_end
@NAME@_chr_end:

    .balign 4
    .global @NAME@_prg
@NAME@_prg:
    .incbin "prg.bin"
    .global @NAME@_prg_end
@NAME@_prg_end:
