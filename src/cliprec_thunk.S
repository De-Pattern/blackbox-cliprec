/*
 * While a take is open on a pad (cliprec.c), the sequence's own note on / note off for that pad are ignored, so an
 * older note under the take cannot restart or cut the clip being played live. Original 1010music Blackbox, 3.1.9.
 *
 * Both pad engines read events from the pad's port and dispatch on the type through a jump table
 * (entry = table + 4 * (type - 2)); r4 = the pad object, its id at +0x18 is (row << 8) | col:
 *   class A (FUN_080596b8, table 0x0805971c, next event 0x080596f8): 0x11 -> 0x0805a734, 0x12 -> 0x0805a6fe
 *   class B (FUN_08067fac, table 0x08068018, next event 0x08067ff4): 0x11 -> 0x080686d2, 0x12 -> 0x080682b2
 * Type 0x11 / 0x12 are the sequencer's note on / off; live taps are 2 / 3 and are not touched.
 * The mute mask lives in patch RAM (cliprec.c), bit = pad index = row * 4 + col. Every case starts by loading r2 and
 * r3, and r12 is scratch, so those three are free here.
 */
    .syntax unified
    .thumb
    .text

    .equ MUTE,       0x2405ff54
    .equ MUTE_MAGIC, 0x434d5554

    .macro seq_event name, stock, next
    .global \name
    .thumb_func
    .type \name, %function
\name:
    ldr     r2, =MUTE
    ldr     r3, [r2]
    ldr     r12, =MUTE_MAGIC
    cmp     r3, r12
    bne     1f
    ldrh    r3, [r2, #4]             @ muted pads
    cbz     r3, 1f
    ldr     r2, [r4, #0x18]          @ (row << 8) | col
    lsrs    r12, r2, #16
    bne     1f
    ubfx    r12, r2, #8, #8          @ row
    cmp     r12, #3
    bhi     1f
    and     r2, r2, #0xff            @ col
    cmp     r2, #3
    bhi     1f
    add     r2, r2, r12, lsl #2      @ pad index
    lsrs    r3, r3, r2
    tst     r3, #1
    beq     1f
    ldr     r3, =\next + 1           @ muted: skip the event
    bx      r3
1:  ldr     r3, =\stock + 1
    bx      r3
    .ltorg
    .endm

    seq_event cliprec_a_on,  0x0805a734, 0x080596f8
    seq_event cliprec_a_off, 0x0805a6fe, 0x080596f8
    seq_event cliprec_b_on,  0x080686d2, 0x08067ff4
    seq_event cliprec_b_off, 0x080682b2, 0x08067ff4
