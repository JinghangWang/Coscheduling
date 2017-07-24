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

#include <nautilus/nautilus.h>
#include <nautilus/scheduler.h>

#include <nautilus/group.h>
#include <nautilus/group_sched.h>

#define DEFAULT_PRIORITY 1

#ifdef NAUT_CONFIG_DEBUG_GROUP_SCHED
#define GROUP_SCHED(fmt, args...)     nk_vc_printf_wrap("CPU %d: " fmt, my_cpu_id(), ##args)
#else
#define GROUP_SCHED(fmt, args...)
#endif

typedef struct group_state {
  struct nk_sched_constraints group_constraints;
  int changing_fail;
  int roll_back_to_old_fail;
  int roll_back_to_default_fail;
  uint64_t changing_count;
} group_state_t;

static group_state_t group_state;
static spinlock_t group_change_constraint_lock;

int
nk_group_sched_init(void) {
  if (memset(&group_state, 0, sizeof(group_state)) == NULL) {
    ERROR_PRINT("Fail to clear memory for group_state!\n");
    return -1;
  }

  spinlock_init(&group_change_constraint_lock);

  return 0;
}

int
nk_group_sched_deinit(void) {
  if (memset(&group_state, 0, sizeof(group_state)) == NULL) {
    ERROR_PRINT("Fail to clear memory for group_state!\n");
    return -1;
  }

  spinlock_deinit(&group_change_constraint_lock);

  return 0;
}

static int
nk_group_sched_set_state(nk_thread_group_t *group, struct nk_sched_constraints *constraints) {
  group_state.group_constraints = *constraints;
  group_state.changing_fail = 0;
  group_state.roll_back_to_old_fail = 0;
  group_state.roll_back_to_default_fail = 0;
  group_state.changing_count = nk_thread_group_get_size(group);

  return 0;
}

static int
nk_group_sched_reset_state(void) {
  int res = 0;

  if (memset(&group_state.group_constraints, 0, sizeof(struct nk_sched_constraints)) == NULL) {
    ERROR_PRINT("Fail to clear memory for group constraints!\n");
    res = 1;
  }

  group_state.changing_fail = 0;
  group_state.roll_back_to_old_fail = 0;
  group_state.roll_back_to_default_fail = 0;
  group_state.changing_count = 0;

  return res;
}

static int
group_roll_back_constraint() {
  struct nk_sched_constraints roll_back_constraints = { .type=APERIODIC,
                                                        .aperiodic.priority=DEFAULT_PRIORITY};

  if (nk_sched_thread_change_constraints(&roll_back_constraints) != 0) {
    return -1;
  }

  return 0;
}

int
nk_group_sched_change_constraints(nk_thread_group_t *group, struct nk_sched_constraints *constraints, uint64_t tid) {
  //store old constraint
  struct nk_thread *t = get_cur_thread();
  struct nk_sched_constraints old;
  nk_sched_thread_get_constraints(t, &old);

  if (nk_thread_group_check_leader(group) == 1) {
    spin_lock(&group_change_constraint_lock);
    nk_group_sched_set_state(group, constraints);
    nk_thread_group_attach_state(group, &group_state);
  }

  nk_thread_group_barrier(group);

  if (group_state.changing_fail == 0) {
    if (nk_sched_thread_change_constraints(&group_state.group_constraints) != 0) {
      //if fail, set the failure flag
      atomic_cmpswap(group_state.changing_fail, 0, 1);
    }
  }

  nk_thread_group_barrier(group);

  int res = 0;
  //check if there is failure, of so, start roll back
  if (group_state.changing_fail) {
    //try to roll back to old constraints first
    GROUP_SCHED("Change constraints failed, roll back to old constraints!\n");
    if (nk_sched_thread_change_constraints(&old) != 0) {
      atomic_cmpswap(group_state.roll_back_to_old_fail, 0, 1);
      GROUP_SCHED("Unable to roll back to old constraints!\n");
    }

    nk_thread_group_barrier(group);

    //if there is any failure, roll back to default constraints
    if (group_state.roll_back_to_old_fail) {
      GROUP_SCHED("Fail to roll back to old constraints, roll back to default constraints!\n");
      if(group_roll_back_constraint() != 0) {
        panic("Roll back to default constraints should not fail!\n");
      }
    }

    res = 1;
  }

  //finally leave this stage and dec counter, if I'm the last one, unlock the group and reset state
  if(atomic_dec_val(group_state.changing_count) == 0) {
    nk_thread_group_detach_state(group);
    nk_group_sched_reset_state();
    spin_unlock(&group_change_constraint_lock);
  }

  return res;
}
