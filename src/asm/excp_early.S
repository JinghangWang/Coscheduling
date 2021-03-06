/*
 * This file is part of the Nautilus AeroKernel developed
 * by the Hobbes and V3VEE Projects with funding from the
 * United States National  Science Foundation and the Department of Energy.
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national
 * laboratories and universities. You can find out more at:
 * http://www.v3vee.org  and
 * http://xtack.sandia.gov/hobbes
 *
 * Copyright (c) 2015, Kyle C. Hale <kh@u.northwestern.edu>
 * Copyright (c) 2015, The V3VEE Project  <http://www.v3vee.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Author: Kyle C. Hale <kh@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */
#include <nautilus/idt.h>
#include <nautilus/thread.h>
// #include <nautilus/scheduler.h>

.code64
.macro make_excp_handler v
    .if (ERR_CODE_EXCP_MASK >> \v) & 1
        pushq $0
    .else
        GEN_NOP(NOP_2BYTE)
    .endif
    pushq $\v
    jmp early_excp_common
    .align 16
.endm

.align 16
GLOBAL(early_excp_handlers)
    vector = 0
    .rept 32
    make_excp_handler vector
    vector = vector + 1
    .endr
END(early_excp_handlers)


.macro make_irq_handler v
    pushq $0 // KCH DANGER
    pushq $\v
    jmp early_irq_common
    .align 16
.endm

.align 16
GLOBAL(early_irq_handlers)
    vector = 32
    .rept NUM_IDT_ENTRIES - 32
    make_irq_handler vector
    vector = vector + 1
    .endr
END(early_irq_handlers)


.align 16
GLOBAL(early_irq_common)
    incw %gs:8   // increment interrupt nesting level -
	         // cpu is now in interrupt context
    incq %gs:16  // increment interrupt count

    SAVE_GPRS()

      // callq sample_time_stamp
    callq irp_enter

#ifdef NAUT_CONFIG_PROFILE
    callq nk_irq_prof_enter
#endif

    leaq 128(%rsp), %rdi # pointer to exception struct
    movq 120(%rsp), %rsi # irq num
    movabs $idt_handler_table, %rdx
    movq (%rdx, %rsi, 8), %r8   # pointer to handler
    movabs $idt_state_table, %rdx
    movq (%rdx, %rsi, 8), %rdx   # pointer to state


#ifdef NAUT_CONFIG_ENABLE_ASSERTS
    cmpq $256, %rsi
    jge irq_err

    cmpq $32, %rsi
    jl irq_err
#endif

    callq *%r8    # rdi = exp struct, rsi = irq num, rdx = state

    testq %rax, %rax
    jnz irq_err

    callq irq_exit
#ifdef NAUT_CONFIG_PROFILE
    callq nk_irq_prof_exit
#endif

    // we're back from the irq handler
    // do we need to switch to someone else?
    callq nk_sched_need_resched
    testq %rax, %rax
    jz thr_return
    movq %rax, %rdi

    pushq %rdi
    call resched_exit //TODO: Question
    popq %rdi

    jmp nk_thread_switch_intr_entry

thr_return:
    RESTORE_GPRS()
    addq $16, %rsp  // pop the vector and the error code
    decw %gs:8      // decrement interrupt nesting level
	            // we may now be outside of interrupt context
    iretq

irq_err:
    movabs $irq_err_str, %rdi
    movabs $panic, %rax
    callq *%rax
errloop:
    hlt
    jmp errloop

.align 8
GLOBAL(early_excp_common)

    SAVE_GPRS()

    incw %gs:8   // increment interrupt nesting level -
	         // cpu is now in interrupt context
    incq %gs:24  // increment exception count

    movq 120(%rsp), %rsi
    cmpq $32, %rsi
    jge excp_err

    leaq 128(%rsp), %rdi # pointer to exception struct
    movq 120(%rsp), %rsi # excp vector
    movabs $idt_handler_table, %rdx
    movq (%rdx, %rsi, 8), %r8   # pointer to handler
    movabs $idt_state_table, %rdx
    movq (%rdx, %rsi, 8), %rcx   # pointer to state
    cmpq $14, %rsi
    jnz no_pf
    movq %cr2, %rdx

no_pf:
    callq *%r8   # rdi = exp struct, rsi = irq num, rdx = error, rcx = state
    testq %rax, %rax
    jnz excp_err

    RESTORE_GPRS()
    /* pop error code and excp number */
    addq $16, %rsp
    decw %gs:8      // decrement interrupt nesting level
	            // we may now be outside of interrupt context
    iretq

excp_err:
    movabs $excp_err_str, %rdi
    movabs $panic, %rax
    callq *%rax
loop1:
    hlt
    jmp loop1


.align 8
GLOBAL(idt_handler_table)
.rept NUM_EXCEPTIONS
    .quad null_excp_handler
.endr

.rept NUM_IDT_ENTRIES - NUM_EXCEPTIONS
    .quad null_irq_handler
.endr
END(idt_handler_table)

.align 8
GLOBAL(idt_state_table)
.rept NUM_IDT_ENTRIES
    .quad 0
.endr
END(idt_state_table)


irq_err_str:
.asciz "Received illegal interrupt %u\12"

excp_err_str:
.asciz "Received illegal exception %u\12"
