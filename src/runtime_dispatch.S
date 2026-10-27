.syntax unified
.arch armv5te
.arm
.text
.align 2

    .hidden runtime_peripheral_dispatch

.global runtime_peripheral_bridge
.type runtime_peripheral_bridge, %function
runtime_peripheral_bridge:
    /*
     * The local trampoline already saved the firmware r12 and lr.  Save the
     * remaining registers and flags before entering ordinary AAPCS C code.
     * r12 contains the per-site metadata generated with the trampoline.
     */
    push    {r0-r11, lr}
    mrs     r2, cpsr
    push    {r2}
    add     r0, sp, #4
    mov     r1, r12
    ldr     r2, [sp]
    /* PC-relative call: an absolute literal pool entry would need an
     * R_ARM_ABS32 relocation, which a shared object cannot use for a
     * preemptible symbol.  blx also lets the linker interwork into Thumb. */
    blx     runtime_peripheral_dispatch
    pop     {r2}
    msr     cpsr_f, r2
    pop     {r0-r11, lr}
    bx      lr
.size runtime_peripheral_bridge, .-runtime_peripheral_bridge
