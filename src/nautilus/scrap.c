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
 * http://xstack.sandia.gov/hobbes
 *
 * Copyright (c) 2017, Xiaoyang Wang <xiaoyangwang2018@u.northwestern.edu>
 *                     Jinghang Wang`<jinghangwang2018@u.northwestern.edu>
 * Copyright (c) 2017, The V3VEE Project  <http://www.v3vee.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Xiaoyang Wang <xiaoyangwang2018@u.northwestern.edu>
 *          Jinghang Wang`<jinghangwang2018@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

 #include <nautilus/scheduler.h>

void nk_thread_queue_sleep_count(nk_thread_queue_t *wq, int *count)
{
    nk_thread_t * t = get_cur_thread();
    uint8_t flags;

    flags = spin_lock_irq_save(&wq->lock);

   t->status = NK_THR_WAITING;
   nk_enqueue_entry(wq, &(t->wait_node));
   atomic_inc(*count);

   __asm__ __volatile__ ("mfence" : : : "memory");

   preempt_disable();

   irq_enable_restore(flags);

   nk_sched_sleep(&wq->lock);

   return;
}
