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
#include <nautilus/cpu.h>
#include <nautilus/naut_assert.h>
#include <nautilus/irq.h>
#include <nautilus/idle.h>
#include <nautilus/paging.h>
#include <nautilus/thread.h>
#include <nautilus/percpu.h>
#include <nautilus/atomic.h>
#include <nautilus/queue.h>
#include <nautilus/list.h>
#include <nautilus/errno.h>
#include <nautilus/mm.h>

#include <nautilus/group.h>

//Group Concept
//
// List implementation for use in scheduler
// Avoids changing thread structures
//
#define TESTER_NUM 4
#define MAX_GROUP_NAME 32
#define MAX_CPU_NUM NAUT_CONFIG_MAX_CPUS

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

typedef struct group_member {
    int tid;  //This is the id in the group
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

    struct nk_sched_constraints group_constraints;  //field; size unknown, shared,last longer than obj
    int changing_constraint;
    int changing_fail;
    uint64_t changing_count;

    uint64_t* dur_dump[TESTER_NUM];

    struct list_head thread_group_node;
} nk_thread_group_t;

typedef struct parallel_thread_group_list
{
    spinlock_t group_list_lock;
    uint64_t num_groups;
    struct list_head group_list_node;
} parallel_thread_group_list_t;

static parallel_thread_group_list_t parallel_thread_group_list; //Malloc at init? pointer or variable

// static spinlock_t group_change_constraint_lock;

//helper functions

//group list
static nk_thread_group_t* group_member_init(nk_thread_group_t *g);
static void               group_member_deinit(nk_thread_group_t *n);
static uint64_t           get_next_group_id(void);

//group barrier
static void             group_barrier_init(nk_barrier_t * barrier);
static void             group_barrier_join(nk_barrier_t * barrier);
static int              group_barrier_leave(nk_barrier_t * barrier);
static int              group_barrier_wait(nk_barrier_t * barrier);

//sleep with a counter, no longer needed
static void             nk_thread_queue_sleep_count(nk_thread_queue_t *wq, int *count);

//print time
static void             group_dur_dump(nk_thread_group_t* g);

//constraints
extern struct nk_sched_constraints* get_rt_constraint(struct nk_thread *t);
int group_roll_back_constraint();

//barrier
static inline void
bspin_lock (volatile int * lock){
  while (__sync_lock_test_and_set(lock, 1));
}

static inline void
bspin_unlock (volatile int * lock) {
  __sync_lock_release(lock);
}


void thread_group_list_init(void) {
    parallel_thread_group_list.num_groups = 0;
    INIT_LIST_HEAD(&parallel_thread_group_list.group_list_node);
    spinlock_init(&parallel_thread_group_list.group_list_lock);
}

int thread_group_list_deinit(void) {
    spinlock_t * lock = &parallel_thread_group_list.group_list_lock;
    spin_lock(lock);
    if (!list_empty(&parallel_thread_group_list.group_list_node)) {
        nk_vc_printf("Can't deinit group list!\n");
        spin_unlock(lock);
        return -1;
    } else {
        if(parallel_thread_group_list.num_groups != 0) {
          panic("num_groups isn't 0 when parallel_thread_group_list is empty!\n");
        }
        spinlock_deinit(&parallel_thread_group_list.group_list_lock);
        spin_unlock(lock);
        return 0;
    }
}

struct nk_thread_group *
nk_thread_group_create(char *name) {
    void *ret = NULL;

    nk_thread_group_t* new_group = (nk_thread_group_t *) MALLOC(sizeof(nk_thread_group_t)); //TODO: Failure handle

    if(new_group == NULL) {
      ERROR_PRINT("Fail to malloc space for group!\n");
      return NULL;
    }

    if(memset(new_group, 0, sizeof(new_group)) == NULL) {
      FREE(new_group);
      ERROR_PRINT("Fail to clear memory for group!\n");
      return NULL;
    }

    ret = strncpy(new_group->group_name,name,MAX_GROUP_NAME);

    if(ret == NULL) {
      FREE(new_group);
      ERROR_PRINT("Fail to copy name for group!\n");
      return NULL;
    }

    for(int i = 0; i < MAX_CPU_NUM; i++) {
      INIT_LIST_HEAD(&new_group->group_member_array[i]);
    }

    new_group->group_leader = -1;
    new_group->group_size = 0;
    new_group->init_fail = 0;
    new_group->next_id = 0;
    // new_group->group_barrier = (nk_barrier_t *) malloc(sizeof(nk_barrier_t));

    new_group->message = NULL;
    new_group->msg_flag = 0;
    new_group->msg_count = 0;

    // new_group->group_constraints = malloc(sizeof(struct nk_sched_constraints));
    new_group->changing_constraint = 0;
    new_group->changing_fail = 0;
    new_group->changing_count = 0;

    INIT_LIST_HEAD(&new_group->thread_group_node);
    list_add(&new_group->thread_group_node, &parallel_thread_group_list.group_list_node);

    spinlock_init(&new_group->group_lock);

    new_group->group_id = get_next_group_id(); //group id is assigned in group_list_enqueue()

    if(new_group->group_id < 0) {
      ERROR_PRINT("Fail to assign group id!\n");
      FREE(new_group);
      return NULL;
    }

     list_add(&new_group->thread_group_node, &parallel_thread_group_list.group_list_node);  //Can it fail?

     group_barrier_init(&new_group->group_barrier);

    return new_group;
}

// delete a group (should be empty)
int
nk_thread_group_delete(nk_thread_group_t *group) {
    if(group->group_size != 0) {
      GROUP("Unable to delete thread group!\n");
      return -1;
    }

    list_del(&group->thread_group_node);

    //The group members have been freed.
    //Free dur dump should be a function call
    for(int i = 0; i < TESTER_NUM; i++) {
      FREE(group->dur_dump[i]);
    }
    FREE(group->message);
    FREE(group);
    return 0;
}

static group_member_t* group_member_create(void)
{
    group_member_t *group_member = (group_member_t *)MALLOC(sizeof(group_member_t));

    if(group_member == NULL) {
      ERROR_PRINT("Fail to malloc space for group member!\n");
      return NULL;
    }

    if(memset(group_member, 0, sizeof(group_member)) == NULL) {
      FREE(group_member);
      ERROR_PRINT("Fail to clear memory for group member!\n");
      return NULL;
    }

    group_member->tid = -1;
    group_member->thread = get_cur_thread();

    INIT_LIST_HEAD(&group_member->group_member_node);

    return group_member;
}

static void group_member_destroy(group_member_t *group_member)
{
    list_del(&group_member->group_member_node);
    FREE(group_member);
}

uint64_t get_next_group_id(void){
    parallel_thread_group_list_t * l = &parallel_thread_group_list;
    if (list_empty(&l->group_list_node)){
        return 0;
    } else {
        nk_thread_group_t *last_member = list_first_entry(&l->group_list_node, nk_thread_group_t, thread_group_node);
        return last_member->group_id + 1;
    }
}

// search for a thread group by name
struct nk_thread_group *
nk_thread_group_find(char *name){
    struct list_head *cur = NULL;
    nk_thread_group_t *cur_group = NULL;

    parallel_thread_group_list_t * l = &parallel_thread_group_list;

    spin_lock(&l->group_list_lock);

    list_for_each(cur, &(parallel_thread_group_list.group_list_node)) {
      list_entry(cur, nk_thread_group_t, thread_group_node);
      if (! (strcmp(cur_group->group_name, name) ))  {
            spin_unlock(&l->group_list_lock);
            return cur_group;
        }
    }

    spin_unlock(&l->group_list_lock);
    return NULL;
}

// current thread joins a group
int
nk_thread_group_join(nk_thread_group_t *group, uint64_t *dur){
    spin_lock(&group->group_lock);

    group_member_t* group_member = group_member_create();

    if(group_member == NULL) {
      ERROR_PRINT("Fail to create group member!\n");
      return -1;
    }

    group_barrier_join(&group->group_barrier);

    group->group_size++;
    int id = group->next_id++;

    list_add(&group_member->group_member_node, &group->group_member_array[my_cpu_id()]);

    group->dur_dump[id] = dur;  //Nasty

    spin_unlock(&group->group_lock);

    return id;
}

// current thread leaves a group
int
nk_thread_group_leave(nk_thread_group_t *group){
    spin_lock(&group->group_lock);
    group->group_size--;
    //remove from thread list

    group_member_t *leaving_member;
    struct nk_thread *cur_thread = get_cur_thread();
    struct list_head *cur;

    list_for_each(cur, &group->group_member_array[my_cpu_id()]) {
      leaving_member = list_entry(cur, group_member_t, group_member_node);
      if(cur_thread == leaving_member->thread) {
        GROUP("Member found in group leave!\n");
      }
    }

    list_del(&leaving_member->group_member_node);
    FREE(&leaving_member);

    spin_unlock(&group->group_lock);

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

// maybe...
// broadcast a message to all members of the thread group be waiting here
// if some threads don't get this message, they just enter the next round
int
nk_thread_group_broadcast(nk_thread_group_t *group, void *message, uint64_t tid, uint64_t src) {
  if(tid != src) {
    //receiver
    int ret = atomic_inc_val(group->msg_count);
    GROUP("msg_count = %d\n", group->msg_count);
    while (group->msg_flag == 0) {
      GROUP("tthread_group_t%d is waiting\n", tid);
    }
    message = group->message;
    GROUP("recv: %s", (char *)message);
    if (atomic_dec_val(group->msg_count) == 0) {
      group->message = NULL;
      group->msg_flag = 0;
      GROUP("Reset msg\n");
    }
    GROUP("msg_count = %d\n", group->msg_count);
  } else {
    //sender
    while(group->msg_flag == 1) {
      GROUP("t%d is sending\n", tid);
    }
    group->message = message;
    group->msg_flag = 1;
    GROUP("Msg sent\n");
    GROUP("send: %s", (char *)message);
  }

  return 0;
}

int
group_change_constraint(nk_thread_group_t *group, int tid) {
  nk_thread_group_barrier(group);

  //store old constraint somewhere
  struct nk_thread *t = get_cur_thread();
  struct nk_sched_constraints *old = get_rt_constraint(t); //needed for retry, implement retry later

  //check if group is locked, if not, lock it
  atomic_cmpswap(group->changing_constraint, 0, 1);

  //inc the counter, check if I'm the last one, if so, wake everyone, if not, go to sleep
  if(atomic_inc_val(group->changing_count) == group->group_size) {
    //check if there is failure, if so, don't do local change constraint
    if (group->changing_fail == 0) {
      if (nk_sched_thread_change_constraints(&group->group_constraints) != 0) {
        //if fail, set the failure flag
        atomic_cmpswap(group->changing_fail, 0, 1);
      }
    }
    nk_thread_group_barrier(group);
  } else {
    //check if there is failure, if so, don't do local change constraint
    if (group->changing_fail == 0) {
      if (nk_sched_thread_change_constraints(&group->group_constraints) != 0) {
        //if fail, set the failure flag
        atomic_cmpswap(group->changing_fail, 0, 1);
      }
    }

    nk_thread_group_barrier(group);
  }

  int res = 0;
  //check if there is failure, of so roll back
  if (group->changing_fail) {
    if(group_roll_back_constraint() != 0) {
      panic("roll back should not fail!\n");
    }
    res = 1;
  }

  //finally leave this stage and dec counter, if I'm the last one, unlock the group
  if(atomic_dec_val(group->changing_count) == 0) {
    atomic_cmpswap(group->changing_constraint, 1, 0);
  }

  return res;
}

#define default_priority 1

int
group_roll_back_constraint() {
  struct nk_sched_constraints roll_back_cons = { .type=APERIODIC,
                                                 .aperiodic.priority=default_priority};

  if(nk_sched_thread_change_constraints(&roll_back_cons) != 0) {
    return -1;
  }

  return 0;
}

//testing
static void group_tester(void *in, void **out){
  uint64_t start, end;
  uint64_t dur[5] = {0, 0, 0, 0, 0};
  GROUP("group name in tester is : %s\n", (char*)in);
  nk_thread_group_t *dst = nk_thread_group_find((char*) in);
  if (!dst){
      GROUP("group_find failed\n");
      return;
  }

  start = rdtsc();
  int tid = nk_thread_group_join(dst, dur);
  end = rdtsc();

  dur[0] = end - start;

  if (tid < 0) {
      GROUP("group join failed\n");
      return;
  } else {
      GROUP("group_join ok, tid is %d\n", tid);
  }
  char name[20];
  sprintf(name, "tester %d", tid);
  nk_thread_name(get_cur_thread(), name);

  int i = 0;
  while(atomic_add(dst->group_size, 0) != TESTER_NUM) {
#ifdef NAUT_CONFIG_DEBUG_THREADS
    // i += 1;
    // if(i == 0xffffff) {
    //   GROUP("group_size = %d\n", atomic_add(dst->group_size, 0));
    //   i = 0;
    // }
#endif
  }

#ifdef NAUT_CONFIG_DEBUG_THREADS
  if(tid == 0) {
    GROUP("All joined!\n");
  }
#endif

  start = rdtsc();
  int leader = nk_thread_group_election(dst, tid);
  end = rdtsc();
  dur[1] = end - start;

  if (leader == tid) {
    /*
    dst->group_constraints->type = APERIODIC;
    dst->group_constraints->interrupt_priority_class = 0x1;
    dst->group_constraints->periodic.phase = 0;
    dst->group_constraints->periodic.period = 10000000000;
    dst->group_constraints->periodic.slice = 100000000;
    */
    dst->group_constraints.type = APERIODIC;
    dst->group_constraints.interrupt_priority_class = 0x1;
  }

  start = rdtsc();
  if(group_change_constraint(dst, tid)) {
    end = rdtsc();
    GROUP("t%d change constraint failed\n", tid);
  } else {
    end = rdtsc();
    // GROUP("t%d change constraint succeeded#\n", tid);
    GROUP("t%d #\n", tid);
  }

  dur[2] = end - start;

  //change_constraint measure
  start = rdtsc();
  nk_sched_thread_change_constraints(&dst->group_constraints);
  end = rdtsc();

  dur[3] = end - start;

  //barrier test
  int ret;
  static int succ_count = 0;
  #define NUM_LOOP 1
  for (i = 0; i< NUM_LOOP; ++i){
      start = rdtsc();
      ret = nk_thread_group_barrier(dst);
      end = rdtsc();
      if (ret){
          atomic_inc(succ_count);
          GROUP("&\n");
      }
  }

  nk_thread_group_barrier(dst);

#ifdef NAUT_CONFIG_DEBUG_THREADS
  if(tid == 0) {
    GROUP("succ_count = %d\n", succ_count);
  }
#endif
  dur[4] = end - start;

  nk_thread_group_barrier(dst);
  if(tid == 0) {
    group_dur_dump(dst);
  }

  nk_thread_group_barrier(dst);

  nk_thread_group_leave(dst);

  nk_thread_group_delete(dst);

  return;

  /*
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

  while(1) {}
  */
}


static int launch_tester(char * group_name, int cpuid) {
    nk_thread_id_t tid;

    if (nk_thread_start(group_tester, (void*)group_name , NULL, 1, PAGE_SIZE_4KB, &tid, cpuid)) {
        return -1;
    } else {
        return 0;
    }
}

int group_test(){
    thread_group_list_init();
    char group_name[20];
    sprintf(group_name, "helloworld!");
    nk_thread_group_t * new_group = nk_thread_group_create(group_name);
    if (new_group != NULL) {
        GROUP("group_create succeeded\n");
    } else {
        GROUP("group_create failed\n");
        return -1;
    }

    nk_thread_group_t * ret = nk_thread_group_find(group_name);
    if (ret != new_group){
        GROUP("result from group_create does not match group_find!\n");
    }
    // launch a few aperiodic threads (testers), i.e. regular threads
    // each join the group
    int i;
    for (i = 0; i < TESTER_NUM; ++i){
        if (launch_tester(group_name, i + 4)){
            GROUP("starting tester failed\n");
        }
    }

    if(thread_group_list_deinit() == 0) {
      return 0;
    } else {
      return -1;
    }
}

void
group_barrier_init (nk_barrier_t * barrier)
{
    GROUP("Initializing group barrier, group barrier at %p, count=%u\n", (void*)barrier, 0);

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
    GROUP("Thread (%p) entering barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        GROUP("Thread (%p): notify\n", (void*)get_cur_thread());
    } else {
        GROUP("Thread (%p): remaining count = %d\n", (void*)get_cur_thread(), barrier->remaining);
        bspin_unlock(&barrier->lock);
        BARRIER_WHILE(barrier->notify != 1);
    }

    if (atomic_inc_val(barrier->remaining) == barrier->init_count) {
      atomic_cmpswap(barrier->notify, 1, 0); //last thread reset notify
      GROUP("Thread (%p): reset notify\n", (void*)get_cur_thread());
      bspin_unlock(&barrier->lock);
    }

    GROUP("Thread (%p) exiting barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    return res;
}

void
group_barrier_join (nk_barrier_t * barrier)
{
    bspin_lock(&barrier->lock);
    GROUP("Thread (%p) joining barrier \n", (void*)get_cur_thread());
    atomic_inc(barrier->init_count);
    atomic_inc(barrier->remaining);
    bspin_unlock(&barrier->lock);
}

int
group_barrier_leave (nk_barrier_t * barrier)
{
    int res = 0;

    GROUP("Thread (%p) leaving barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    bspin_lock(&barrier->lock);

    atomic_dec(barrier->init_count);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        GROUP("Thread (%p): notify\n", (void*)get_cur_thread());
    }

    bspin_unlock(&barrier->lock);

    return res;
}

//queue
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

static void group_dur_dump(nk_thread_group_t* group) {
  for(int i = 0; i < TESTER_NUM; i++) {
    nk_vc_printf("--For tester %d:\njoin dur = %d\nelection dur = %d\ngroup_change_cons dur = %d\nchange_cons dur = %d\nbarrier dur = %d\n\n",
                  i, group->dur_dump[i][0], group->dur_dump[i][1], group->dur_dump[i][2], group->dur_dump[i][3], group->dur_dump[i][4]);
  }
}

void change_cons_profile() {
  uint64_t start, end;
  int integer;

  start = rdtsc();
  uint64_t test = rdtsc();
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);

  start = rdtsc();
  struct nk_thread *t = get_cur_thread();
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);

  start = rdtsc();
  struct nk_sched_constraints *old = get_rt_constraint(t); //needed for retry, implement retry later
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);

  start = rdtsc();
  atomic_cmpswap(integer, 0, 1);
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);

  start = rdtsc();
  integer = atomic_inc_val(integer);
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);

  start = rdtsc();
  integer = atomic_dec_val(integer);
  end = rdtsc();
  GROUP("Overhead = %d\n", end - start);
}
