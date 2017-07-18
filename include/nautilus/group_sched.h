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

#ifndef _GROUP_SCHED_H_
#define _GROUP_SCHED_H_

#include <nautilus/group.h>

int nk_group_sched_init(void);

int nk_group_sched_deinit(void);

int nk_group_sched_change_constraints(nk_thread_group_t *group,
                                     struct nk_sched_constraints *group_constraints,
                                     uint64_t tid);

#endif /* _GROUP_SCHED_H_ */
