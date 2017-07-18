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
 * Authors:  Xiaoyang Wang <xiaoyangwang2018@u.northwestern.edu>
 *           Jinghang Wang`<jinghangwang2018@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

#include <nautilus/nautilus.h>
#include <nautilus/thread.h>
#include <nautilus/atomic.h>
#include <nautilus/list.h>

#include <nautilus/group.h>
#include <nautilus/group_sched.h>

#define MAX_GROUP_NAME 32
#define MAX_CPU_NUM NAUT_CONFIG_MAX_CPUS

#define TESTS 1

#if TESTS
#define TESTER_NUM 4
#define BARRIER_TEST_LOOPS 1
#endif

#define SANITY_CHECKS 0

#if SANITY_CHECKS
#define PAD 0
#define MALLOC(x) ({ void *p  = malloc((x)+2*PAD); if (!p) { panic("Failed to Allocate %d bytes\n",x); } memset(p,0,(x)+2*PAD); p+PAD; })
#define FREE(x) do {if (x) { free(x-PAD); x=0;} } while (0)
#else // no sanity checks
#define MALLOC(x) malloc(x)
#define FREE(x) free(x)
#endif // sanity checks

#ifdef NAUT_CONFIG_DEBUG_GROUP
#define GROUP(fmt, args...)     nk_vc_printf_wrap("CPU %d: " fmt, my_cpu_id(), ##args)
#else
#define GROUP(fmt, args...)
#endif

#ifdef NAUT_CONFIG_DEBUG_GROUP_BARRIER
#define GROUP_BARRIER(fmt, args...)     nk_vc_printf_wrap("CPU %d: " fmt, my_cpu_id(), ##args)
#else
#define GROUP_BARRIER(fmt, args...)
#endif

typedef struct group_member {
    int tid;  //Important: This is the id in the group, not in the scheduler
    nk_thread_t *thread;
    struct list_head group_member_node;
} group_member_t;

typedef struct nk_thread_group
{
    char group_name[MAX_GROUP_NAME];
    uint64_t group_id;
    uint64_t group_leader;
    uint64_t group_size;
    uint64_t next_id;

    struct list_head group_member_array[MAX_CPU_NUM];

    int init_fail;
    nk_barrier_t group_barrier;
    spinlock_t group_lock;

    void *message;
    int   msg_flag;
    uint64_t msg_count;

    void *state;

#if TESTS
    uint64_t** dur_dump;
#endif

    struct list_head thread_group_node;
} nk_thread_group_t;

typedef struct parallel_thread_group_list
{
    spinlock_t group_list_lock;
    uint64_t num_groups;
    struct list_head group_list_node;
} parallel_thread_group_list_t;

static parallel_thread_group_list_t parallel_thread_group_list; //Malloc at init? pointer or variable

// group member helpers
static nk_thread_group_t* group_member_init(nk_thread_group_t *g);
static void               group_member_deinit(nk_thread_group_t *n);
static uint64_t           get_next_group_id(void);

// group barrier helpers
static void             group_barrier_init(nk_barrier_t * barrier);
static void             group_barrier_join(nk_barrier_t * barrier);
static int              group_barrier_leave(nk_barrier_t * barrier);
static int              group_barrier_wait(nk_barrier_t * barrier);

// barrier lock helpers
static inline void
bspin_lock (volatile int * lock) {
  while (__sync_lock_test_and_set(lock, 1));
}

static inline void
bspin_unlock (volatile int * lock) {
  __sync_lock_release(lock);
}

/******************************************************************************/

// init the global group list
// should be called in system init
static void thread_group_list_init(void) {
    parallel_thread_group_list.num_groups = 0;
    INIT_LIST_HEAD(&parallel_thread_group_list.group_list_node);
    spinlock_init(&parallel_thread_group_list.group_list_lock);
}

// deinit the global group list
// hasn't been used so far
static int thread_group_list_deinit(void) {
    spinlock_t * lock = &parallel_thread_group_list.group_list_lock;
    spin_lock(lock);
    if (!list_empty(&parallel_thread_group_list.group_list_node)) {
        GROUP("Can't deinit group list!\n");
        spin_unlock(lock);
        return -1;
    } else {
        if (parallel_thread_group_list.num_groups != 0) {
          panic("The num_groups isn't 0 when parallel_thread_group_list is empty!\n");
        }
        spinlock_deinit(&parallel_thread_group_list.group_list_lock);
        spin_unlock(lock);
        return 0;
    }
}

// create a group and init it
struct nk_thread_group *
nk_thread_group_create(char *name) {
    void *ret = NULL;

    nk_thread_group_t* new_group = (nk_thread_group_t *) MALLOC(sizeof(nk_thread_group_t));

    if (new_group == NULL) {
      ERROR_PRINT("Fail to malloc space for group!\n");
      return NULL;
    }

    if (memset(new_group, 0, sizeof(new_group)) == NULL) {
      FREE(new_group);
      ERROR_PRINT("Fail to clear memory for group!\n");
      return NULL;
    }

#if TESTS
    new_group->dur_dump = (uint64_t**)MALLOC(TESTER_NUM*sizeof(uint64_t *));

    if (new_group->dur_dump == NULL) {
      FREE(new_group->dur_dump);
      FREE(new_group);
      ERROR_PRINT("Fail to malloc memory for dur_dump!\n");
      return NULL;
    }

    if (memset(new_group->dur_dump, 0, sizeof(TESTER_NUM*sizeof(uint64_t *))) == NULL) {
      FREE(new_group->dur_dump);
      FREE(new_group);
      ERROR_PRINT("Fail to clear memory for dur_dump!\n");
      return NULL;
    }
#endif

    ret = strncpy(new_group->group_name,name,MAX_GROUP_NAME);

    if (ret == NULL) {

#if TESTS
      FREE(new_group->dur_dump);
#endif

      FREE(new_group);
      ERROR_PRINT("Fail to copy name for group!\n");
      return NULL;
    }

    for (int i = 0; i < MAX_CPU_NUM; i++) {
      INIT_LIST_HEAD(&new_group->group_member_array[i]);
    }

    new_group->group_leader = -1;
    new_group->group_size = 0;
    new_group->init_fail = 0;
    new_group->next_id = 0;

    new_group->message = NULL;
    new_group->msg_flag = 0;
    new_group->msg_count = 0;

    new_group->state = NULL;

    INIT_LIST_HEAD(&(new_group->thread_group_node));

    list_add(&(new_group->thread_group_node), &(parallel_thread_group_list.group_list_node));

    spinlock_init(&new_group->group_lock);

    new_group->group_id = get_next_group_id();

    if (new_group->group_id < 0) {
      ERROR_PRINT("Fail to assign group id!\n");

#if TESTS
      FREE(new_group->dur_dump);
#endif

      FREE(new_group);
      return NULL;
    }

    group_barrier_init(&new_group->group_barrier);

    return new_group;
}

// attach a state to the group
int nk_thread_group_attach_state(struct nk_thread_group *group, void *state) {
  group->state = state;

  return 0;
}

// detach the state of a group
void *nk_thread_group_detach_state(struct nk_thread_group *group) {
  group->state = NULL;

  return 0;
}

// get the schedule state of cur group
void *nk_thread_group_get_state(struct nk_thread_group *group) {
  if (group) {
    return group->state;
  } else {
    return 0;
  }
}

// delete a group (the group should be empty)
int
nk_thread_group_delete(nk_thread_group_t *group) {
    if (group->group_size != 0) {
      GROUP("Unable to delete thread group!\n");
      return -1;
    }

    list_del(&group->thread_group_node);

#if TESTS
    for (int i = 0; i < TESTER_NUM; i++) {
      FREE(group->dur_dump[i]);
    }
#endif

    //All group members should have been freed.
    FREE(group->message);
    FREE(group);
    return 0;
}

// create a group member and init it
static group_member_t* group_member_create(void)
{
    group_member_t *group_member = (group_member_t *)MALLOC(sizeof(group_member_t));

    if (group_member == NULL) {
      ERROR_PRINT("Fail to malloc space for group member!\n");
      return NULL;
    }

    if (memset(group_member, 0, sizeof(group_member)) == NULL) {
      FREE(group_member);
      ERROR_PRINT("Fail to clear memory for group member!\n");
      return NULL;
    }

    group_member->tid = -1;
    group_member->thread = get_cur_thread();

    INIT_LIST_HEAD(&group_member->group_member_node);

    return group_member;
}

// delete the group member from list and free it
static void group_member_destroy(group_member_t *group_member)
{
    list_del(&group_member->group_member_node);
    FREE(group_member);
}

// assign a new id for a new group
static uint64_t get_next_group_id(void) {
    parallel_thread_group_list_t * l = &parallel_thread_group_list;
    if (list_empty(&l->group_list_node)) {
        return 0;
    } else {
        nk_thread_group_t *last_member = list_first_entry(&l->group_list_node, nk_thread_group_t, thread_group_node);
        return last_member->group_id + 1;
    }
}

// search for a thread group by name
struct nk_thread_group *
nk_thread_group_find(char *name) {
    struct list_head *cur = NULL;
    nk_thread_group_t *cur_group = NULL;
    parallel_thread_group_list_t * l = &parallel_thread_group_list;

    spin_lock(&l->group_list_lock);

    list_for_each(cur, (struct list_head *)&parallel_thread_group_list.group_list_node) {
      cur_group = list_entry(cur, nk_thread_group_t, thread_group_node);
      if (! (strcmp(cur_group->group_name, name) ))  {
            spin_unlock(&l->group_list_lock);
            return cur_group;
        }
    }

    spin_unlock(&l->group_list_lock);
    return NULL;
}

// return the size of a group
uint64_t
nk_thread_group_get_size(struct nk_thread_group *group) {
  return group->group_size;
}

// return the leader of a group
uint64_t
nk_thread_group_get_leader(struct nk_thread_group *group) {
  return group->group_leader;
}

// current thread joins a group with test log
int
nk_thread_group_join_test(nk_thread_group_t *group, uint64_t *dur) {
    group_member_t* group_member = group_member_create();

    if (group_member == NULL) {
      ERROR_PRINT("Fail to create group member!\n");
      return -1;
    }

    group_barrier_join(&group->group_barrier);

    atomic_inc(group->group_size);
    int id = atomic_inc(group->next_id);

    spin_lock(&group->group_lock);
    list_add(&group_member->group_member_node, &group->group_member_array[my_cpu_id()]);
    spin_unlock(&group->group_lock);

#if TESTS
    group->dur_dump[id] = dur;
#endif

    return id;
}

// current thread joins a group
// please make sure TESTS is set to 0, if you are using this function
int
nk_thread_group_join(nk_thread_group_t *group) {
  return nk_thread_group_join_test(group, NULL);
}

// current thread leaves a group
int
nk_thread_group_leave(nk_thread_group_t *group) {
    atomic_dec(group->group_size);

    group_member_t *leaving_member;
    struct nk_thread *cur_thread = get_cur_thread();
    struct list_head *cur;

    spin_lock(&group->group_lock);

    list_for_each(cur, &group->group_member_array[my_cpu_id()]) {
      leaving_member = list_entry(cur, group_member_t, group_member_node);
      if (cur_thread == leaving_member->thread) {
        break;
      }
    }

    if (cur == &group->group_member_array[my_cpu_id()]) {
      ERROR_PRINT("Fail to find leaving member in group_member_array!\n");
      spin_unlock(&group->group_lock);
      group_barrier_leave(&group->group_barrier);
      return -1;
    }

    list_del(&leaving_member->group_member_node);

    spin_unlock(&group->group_lock);

    FREE(&leaving_member);

    group_barrier_leave(&group->group_barrier);

    return 0;
}

// all threads in the group call to synchronize
int
nk_thread_group_barrier(nk_thread_group_t *group) {
    return group_barrier_wait(&group->group_barrier);
}

// all threads in the group call to select one thread as leader
uint64_t
nk_thread_group_election(nk_thread_group_t *group, uint64_t my_tid) {
    uint64_t leader = atomic_cmpswap(group->group_leader, -1, my_tid);
    if (leader == -1){
        leader = my_tid;
    }

    return leader;
}

// broadcast a message to all members of the thread group be waiting here
// if some threads don't get this message, they just enter the next round
int
nk_thread_group_broadcast(nk_thread_group_t *group, void *message, uint64_t tid, uint64_t src) {
  if (tid != src) {
    //receiver
    int ret = atomic_inc_val(group->msg_count);
    GROUP("msg_count = %d\n", group->msg_count);
    while (group->msg_flag == 0) {
      GROUP("t%d is waiting\n", tid);
    }
    message = group->message;
    GROUP("Recv: %s", (char *)message);
    if (atomic_dec_val(group->msg_count) == 0) {
      group->message = NULL;
      group->msg_flag = 0;
      GROUP("Reset msg\n");
    }
    GROUP("msg_count = %d\n", group->msg_count);
  } else {
    //sender
    while (group->msg_flag == 1) {
      GROUP("t%d is sending\n", tid);
    }
    group->message = message;
    group->msg_flag = 1;
    GROUP("Msg sent\n");
    GROUP("Send: %s", (char *)message);
  }

  return 0;
}

void
group_barrier_init (nk_barrier_t * barrier)
{
    GROUP_BARRIER("Initializing group barrier, group barrier at %p, count=%u\n", (void*)barrier, 0);

    memset(barrier, 0, sizeof(nk_barrier_t));
    barrier->lock = 0;
    barrier->notify = 0;
    barrier->init_count = 0;
    barrier->remaining  = 0;
}

int
group_barrier_wait (nk_barrier_t * barrier)
{
    int res = 0;

    bspin_lock(&barrier->lock);
    GROUP_BARRIER("Thread (%p) entering barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        GROUP_BARRIER("Thread (%p): notify\n", (void*)get_cur_thread());
    } else {
        GROUP_BARRIER("Thread (%p): remaining count = %d\n", (void*)get_cur_thread(), barrier->remaining);
        bspin_unlock(&barrier->lock);
        BARRIER_WHILE (barrier->notify != 1);
    }

    if (atomic_inc_val(barrier->remaining) == barrier->init_count) {
      atomic_cmpswap(barrier->notify, 1, 0); //last thread reset notify
      GROUP_BARRIER("Thread (%p): reset notify\n", (void*)get_cur_thread());
      bspin_unlock(&barrier->lock);
    }

    GROUP_BARRIER("Thread (%p) exiting barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    return res;
}

void
group_barrier_join (nk_barrier_t * barrier)
{
    bspin_lock(&barrier->lock);
    GROUP_BARRIER("Thread (%p) joining barrier \n", (void*)get_cur_thread());
    atomic_inc(barrier->init_count);
    atomic_inc(barrier->remaining);
    bspin_unlock(&barrier->lock);
}

int
group_barrier_leave (nk_barrier_t * barrier)
{
    int res = 0;

    GROUP_BARRIER("Thread (%p) leaving barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    bspin_lock(&barrier->lock);

    atomic_dec(barrier->init_count);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        GROUP_BARRIER("Thread (%p): notify\n", (void*)get_cur_thread());
    }

    bspin_unlock(&barrier->lock);

    return res;
}

int nk_thread_group_init(void) {
  GROUP("Inited\n");

  thread_group_list_init();

  return 0;
}

int nk_thread_group_deinit(void) {
  GROUP("Deinited\n");

  return 0;
}

#if TESTS

static void group_dur_dump(nk_thread_group_t* group) {
  for (int i = 0; i < TESTER_NUM; i++) {
    nk_vc_printf("%d,%d,%d,%d,%d,%d\n",
                  i, group->dur_dump[i][0], group->dur_dump[i][1], group->dur_dump[i][2], group->dur_dump[i][3], group->dur_dump[i][4]);
  }
}

void change_cons_profile() {
  uint64_t start, end;
  int integer;
  struct nk_sched_constraints constraints;

  start = rdtsc();
  uint64_t test = rdtsc();
  end = rdtsc();
  GROUP("rdtsc() overhead = %d\n", end - start);

  start = rdtsc();
  struct nk_thread *t = get_cur_thread();
  end = rdtsc();
  GROUP("get_cur_thread() overhead = %d\n", end - start);

  start = rdtsc();
  nk_sched_thread_get_constraints(t, &constraints);
  end = rdtsc();
  GROUP("nk_sched_thread_get_constraints() overhead = %d\n", end - start);

  start = rdtsc();
  atomic_cmpswap(integer, 0, 1);
  end = rdtsc();
  GROUP("atomic_cmpswap() overhead = %d\n", end - start);

  start = rdtsc();
  integer = atomic_inc_val(integer);
  end = rdtsc();
  GROUP("atomic_inc_val() overhead = %d\n", end - start);

  start = rdtsc();
  integer = atomic_dec_val(integer);
  end = rdtsc();
  GROUP("atomic_dec_val() overhead = %d\n", end - start);
}

static void group_tester(void *in, void **out) {
  uint64_t dur[5] = {0, 0, 0, 0, 0};

  uint64_t start, end;

  static struct nk_sched_constraints *constraints;

  nk_thread_group_t *dst = nk_thread_group_find((char*) in);

  if (!dst) {
      GROUP("group_find failed\n");
      return;
  }

  start = rdtsc();
  int tid = nk_thread_group_join_test(dst, dur);
  end = rdtsc();

  dur[0] = end - start;

  if (tid < 0) {
      GROUP("group join failed\n");
      return;
  }

  char *name = (char *)MALLOC(32*sizeof(char));
  if (name == NULL) {
    GROUP("Fail to malloc space for tester name!\n");
    return;
  }

  sprintf(name, "tester %d", tid);
  nk_thread_name(get_cur_thread(), name);

  int i = 0;
  while (atomic_add(dst->group_size, 0) != TESTER_NUM) {
#ifdef NAUT_CONFIG_DEBUG_GROUP
    i += 1;
    if (i == 0xffffff) {
      GROUP("group_size = %d\n", atomic_add(dst->group_size, 0));
      i = 0;
    }
#endif
  }

#ifdef NAUT_CONFIG_DEBUG_GROUP
  if (tid == 0) {
    GROUP("All joined!\n");
  }
#endif

  start = rdtsc();
  int leader = nk_thread_group_election(dst, tid);
  end = rdtsc();
  dur[1] = end - start;

  if (leader == tid) {
    constraints = MALLOC(sizeof(struct nk_sched_constraints));
    constraints->type = APERIODIC;
    constraints->interrupt_priority_class = 0x01;
  }

  start = rdtsc();
  if (nk_group_sched_change_constraints(dst, constraints, tid)) {
    end = rdtsc();
    GROUP("t%d change constraint failed!\n", tid);
  } else {
    end = rdtsc();
    GROUP("t%d #\n", tid);
  }

  dur[2] = end - start;

  //change_constraint measure
  start = rdtsc();
  nk_sched_thread_change_constraints(constraints);
  end = rdtsc();

  dur[3] = end - start;

  //barrier test
  int ret;
  static int succ_count = 0;
  for (i = 0; i< BARRIER_TEST_LOOPS; ++i) {
      start = rdtsc();
      ret = nk_thread_group_barrier(dst);
      end = rdtsc();
      if (ret) {
          atomic_inc(succ_count);
          GROUP("t%d &\n", tid);
      }
  }

  nk_thread_group_barrier(dst);

#ifdef NAUT_CONFIG_DEBUG_GROUP
  if (tid == 0) {
    GROUP("succ_count = %d\n", succ_count);
  }
#endif
  dur[4] = end - start;

  nk_thread_group_barrier(dst);

  if (tid == 0) {
    group_dur_dump(dst);
  }

  nk_thread_group_barrier(dst);

  nk_thread_group_leave(dst);

  if (tid == leader) {
    nk_thread_group_delete(dst);
    FREE(in);
  }

  return;

  char *msg_0;
  char *msg_1;

  if (tid == 1) {
    msg_0 = "Hello\n";
  }

  if (tid == 0) {
    msg_1 = "World\n";
  }

  GROUP("t%d is here\n", tid);

  nk_thread_group_broadcast(dst, (void *)msg_0, tid, 1);

  GROUP("t%d says %s\n", tid, msg_0);

  nk_thread_group_broadcast(dst, (void *)msg_1, tid, 0);

  GROUP("t%d says %s\n", tid, msg_1);

  while (1) {}

}


static int launch_tester(char * group_name, int cpuid) {
    nk_thread_id_t tid;

    if (nk_thread_start(group_tester, (void*)group_name , NULL, 1, PAGE_SIZE_4KB, &tid, cpuid)) {
        return -1;
    } else {
        return 0;
    }
}

void group_test_0() {
    char* group_name = MALLOC(32*sizeof(char));
    if (!group_name) {
        GROUP("malloc group name faield");
        return;
    }

    sprintf(group_name, "Group 0");
    nk_thread_group_t * new_group = nk_thread_group_create(group_name);
    if (new_group != NULL) {
        GROUP("group_create succeeded\n");
    } else {
        GROUP("group_create failed\n");
        return;
    }

    nk_thread_group_t * ret = nk_thread_group_find(group_name);
    if (ret != new_group) {
        GROUP("result from group_create does not match group_find!\n");
    }

    // launch a few aperiodic threads (testers), i.e. regular threads
    // each join the group
    int i;
    for (i = 0; i < TESTER_NUM; ++i) {
        if (launch_tester(group_name, i)) {
            GROUP("starting tester failed\n");
        }
    }

    return;
}

void group_test_1() {
    char* group_name = MALLOC(32*sizeof(char));
    if (!group_name) {
        GROUP("malloc group name faield");
        return;
    }

    sprintf(group_name, "Group 1");
    nk_thread_group_t * new_group = nk_thread_group_create(group_name);
    if (new_group != NULL) {
        GROUP("group_create succeeded\n");
    } else {
        GROUP("group_create failed\n");
        return;
    }

    nk_thread_group_t * ret = nk_thread_group_find(group_name);
    if (ret != new_group) {
        GROUP("result from group_create does not match group_find!\n");
    }

    // launch a few aperiodic threads (testers), i.e. regular threads
    // each join the group
    int i;
    for (i = 0; i < TESTER_NUM; ++i) {
        if (launch_tester(group_name, i + 4)) {
            GROUP("starting tester failed\n");
        }
    }

    return;
}

int double_group_test() {
    nk_thread_id_t tid_0;
    nk_thread_id_t tid_1;

    if (nk_thread_start(group_test_0, NULL , NULL, 1, PAGE_SIZE_4KB, &tid_0, 0)) {
        ERROR_PRINT("Lanuch group_test_0 failed\n");
    }

    if (nk_thread_start(group_test_1, NULL , NULL, 1, PAGE_SIZE_4KB, &tid_1, 4)) {
        ERROR_PRINT("Lanuch group_test_1 failed\n");
    }

    return 0;
}

int
group_test_lanucher() {
  uint64_t i = 0;

  char* group_name = (char *)MALLOC(32*sizeof(char));
  if (group_name == NULL) {
      GROUP("malloc group name failed\n");
      FREE(group_name);
      return -1;
  }

  memset(group_name, 0, 32*sizeof(char));

  nk_thread_group_t *new_group = NULL;
  nk_thread_group_t *ret = NULL;
  nk_thread_id_t *tids = (nk_thread_id_t *)MALLOC(256*sizeof(nk_thread_id_t));

  if (tids == NULL) {
    GROUP("malloc tids failed\n");
    FREE(tids);
    return -1;
  }

  memset(tids, 0, 256*sizeof(nk_thread_id_t));

  sprintf(group_name, "Group Alpha");

  new_group = nk_thread_group_create(group_name);

  if (new_group == NULL) {
    GROUP("group_create failed\n");
    return -1;
  }

  ret = nk_thread_group_find(group_name);

  if (ret != new_group) {
      GROUP("result from group_create does not match group_find!\n");
  }

  // launch a few aperiodic threads (testers), i.e. regular threads
  // each join the group
  for (i = 0; i < TESTER_NUM; i++) {
    if (nk_thread_start(group_tester, (void*)group_name , NULL, 1, PAGE_SIZE_4KB, &tids[i], i + 4)) {
      GROUP("Fail to start group_tester %d\n", i);
    }
  }

  for (i = 0; i < TESTER_NUM; i++) {
    if (nk_join(tids[i], NULL)) {
      GROUP("Fail to join group_tester %d\n", i);
    }
  }

  FREE(group_name);
  FREE(tids);
  return 0;
}

int tester_num;

int
group_test() {
  nk_vc_printf("Warm Up\n");
  tester_num = TESTER_NUM;
  group_test_lanucher();

  for (int i = 1; i < TESTER_NUM + 1; i++) {
    nk_vc_printf("Round: %d\n", i);
    tester_num = i;
    group_test_lanucher();
  }
  nk_vc_printf("Test Finished\n");

  return 0;
}
#endif /* TESTS */
