// The corpus programs' entry (dm/tools/aot/corpus): an NRO's start as libnx's switch_crt0.s lays it out (a branch over the 0x80 bytes the NRO
// header takes, the MOD0 offset at +4, MOD0 itself in the text after the code), with no libnx behind it. It sets SP to a stack of its own in bss
// (every executor then runs the same addresses), relocates the module unless a loader did (rt.c, rt_relocate), points tpidr_el0 at the main
// thread's TLS block as the AArch64 local-exec model reads it, runs DT_INIT and the init array, calls main and leaves through svcExitProcess
// (rt_exit). The loader hands over a zeroed bss, as every NRO loader does from the header's bss size.
    .section .crt0, "ax", %progbits
    .global _start
    .type   _start, %function
    .align  2
_start:
    b       1f
    .word   __nx_mod0 - _start
    .ascii  "HOMEBREW"
    .org    _start + 0x80
1:
    adrp    x9, rt_stack_top
    add     x9, x9, :lo12:rt_stack_top
    mov     sp, x9
    mov     x29, #0
    adr     x0, _start
    adrp    x1, _DYNAMIC
    add     x1, x1, :lo12:_DYNAMIC
    bl      rt_relocate
    bl      rt_tls_init
    msr     tpidr_el0, x0
    bl      rt_init
    mov     w0, #0
    mov     x1, #0
    bl      main
    b       rt_exit
    .size   _start, . - _start

// MOD0: the offsets of the dynamic section, bss, .eh_frame_hdr and the module object (none), from MOD0 itself (loader.c parse_dynamic)
    .align  2
    .global __nx_mod0
    .hidden __nx_mod0
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __nx_mod0
    .word   __bss_start__ - __nx_mod0
    .word   __bss_end__ - __nx_mod0
    .word   __eh_frame_hdr_start - __nx_mod0
    .word   __eh_frame_hdr_end - __nx_mod0
    .word   0

// The supervisor calls the corpus makes (dm/host/hle_svc.c answers both)
    .text
    .global rt_svc_output_debug_string
    .type   rt_svc_output_debug_string, %function
    .align  2
rt_svc_output_debug_string:          // x0 = string, x1 = length
    svc     #0x27
    ret
    .size   rt_svc_output_debug_string, . - rt_svc_output_debug_string

    .global rt_svc_exit_process
    .type   rt_svc_exit_process, %function
    .align  2
rt_svc_exit_process:
    svc     #0x7
    brk     #0x7
    .size   rt_svc_exit_process, . - rt_svc_exit_process

    .section .bss.rt_stack, "aw", %nobits
    .align  4
rt_stack:
    .space  0x10000
    .global rt_stack_top
    .hidden rt_stack_top
rt_stack_top:
