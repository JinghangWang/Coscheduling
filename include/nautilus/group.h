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

#ifndef _GROUP_H_
#define _GROUP_H_

struct nk_thread_group;
typedef struct nk_thread_group nk_thread_group_t;

// creating a thread group is done as easily as making a name
struct nk_thread_group *nk_thread_group_create(char *name);

// attach a state to the group
int nk_thread_group_attach_state(struct nk_thread_group *group, void *state);

// detach the state of a group
void *nk_thread_group_detach_state(struct nk_thread_group *group);

// recover the state of a group
void *nk_thread_group_get_state(struct nk_thread_group *group);

// search for a thread group by name
struct nk_thread_group *nk_thread_group_find(char *name);

// return the size of a group
uint64_t nk_thread_group_get_size(struct nk_thread_group *group);

// return the leader of a group
uint64_t nk_thread_group_get_leader(struct nk_thread_group *group);

// current thread joins a group
int                     nk_thread_group_join(struct nk_thread_group *group);

// current thread leaves a group
int                     nk_thread_group_leave(struct nk_thread_group *group);

// all threads in the group call to synchronize
int                     nk_thread_group_barrier(struct nk_thread_group *group);

// all threads in the group call to select one thread as leader
uint64_t                nk_thread_group_election(struct nk_thread_group *group, uint64_t my_tid);

// broadcast a message to all members of the thread group
static int              nk_thread_group_broadcast(struct nk_thread_group *group, void *message, uint64_t tid, uint64_t src);

// delete a group (should be empty)
int                     nk_thread_group_delete(struct nk_thread_group *group);

// init of module
int nk_thread_group_init(void);

// deinit of module
int nk_thread_group_deinit(void);

#endif /* _GROUP_H */
