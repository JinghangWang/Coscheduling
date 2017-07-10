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

#include <nautilus/parthread.h>

//Group Concept
//
// List implementation for use in scheduler
// Avoids changing thread structures
//
#define TESTER_NUM 2

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

#define MAX_CPU_NUM     100

typedef struct thread_unit {
    int tid;
    nk_thread_t *thread;
    struct thread_unit *next;
    struct thread_unit *prev;
} thread_unit;

typedef struct nk_thread_group
{
    char *group_name;
    uint64_t group_id;
    uint64_t group_leader;
    uint64_t group_size;
    uint64_t next_id;

    thread_unit* thread_unit_list[MAX_CPU_NUM]; //May not need, has not been implemented

    int init_fail;
    nk_barrier_t* group_barrier;
    spinlock_t group_lock;

    void *message;
    int   msg_flag;
    uint64_t msg_count;

    struct nk_sched_constraints *group_constraints;
    int changing_constraint;
    int changing_fail;
    uint64_t changing_count;

    nk_thread_queue_t *change_cons_wait_q;
    int sleep_count;

    uint64_t* dur_dump[TESTER_NUM];
} nk_thread_group;

typedef struct group_node {
    nk_thread_group        *group;
    struct group_node *next;
    struct group_node *prev;
} group_node;

typedef struct parallel_thread_group_list
{
    spinlock_t group_list_lock;
    uint64_t num_groups;
    group_node *head;
    group_node *tail;
} parallel_thread_group_list_t;

static parallel_thread_group_list_t parallel_thread_group_list; //Malloc at init

//helper functions

//group list
static int              group_list_enqueue(nk_thread_group* g);
static nk_thread_group* group_list_remove(nk_thread_group* g);
static int              group_list_empty(void);
static group_node*      group_node_init(nk_thread_group *g);
static void             group_node_deinit(group_node *n);
static uint64_t         get_next_groupid(void);

//group barrier
static int              group_barrier_init(nk_barrier_t * barrier);
static int              group_barrier_join(nk_barrier_t * barrier);
static int              group_barrier_leave(nk_barrier_t * barrier);
static int              group_barrier_wait(nk_barrier_t * barrier);

//sleep with a counter
static void             nk_thread_queue_sleep_count(nk_thread_queue_t *wq, int *count);

//print time
static void             group_dur_dump(nk_thread_group* g);

static void group_dur_dump(nk_thread_group* group) {
  for(int i = 0; i < TESTER_NUM; i++) {
    nk_vc_printf("--For tester %d:\njoin dur = %d\nelection dur = %d\ngroup_change_cons dur = %d\nchange_cons dur = %d\nbarrier dur = %d\n\n",
                  i, group->dur_dump[i][0], group->dur_dump[i][1], group->dur_dump[i][2], group->dur_dump[i][3], group->dur_dump[i][4]);
  }
}

int nk_thread_group_init(void) {
    parallel_thread_group_list.num_groups = 0;
    parallel_thread_group_list.head = NULL;
    parallel_thread_group_list.tail = NULL;
    spinlock_init(&parallel_thread_group_list.group_list_lock);
    return 0;
}

int nk_thread_group_deinit(void) {
    spinlock_t * lock = &parallel_thread_group_list.group_list_lock;
    spin_lock(lock);
    if (!group_list_empty()) {
        nk_vc_printf("Can't deinit group list\n");
        spin_unlock(lock);
        return -1;
    } else {
        //parallel_thread_group_list.head = NULL;
        //parallel_thread_group_list.tail = NULL;
        spin_unlock(lock);
        return 0;
    }
}

static group_node* group_node_init(nk_thread_group *g)
{
    group_node *node = (group_node *)MALLOC(sizeof(group_node));
    if (node) {
        node->group = g;
        node->next = NULL;
        node->prev = NULL;
    }
    return node;
}

static void group_node_deinit(group_node *n)
{
    FREE(n);
}

static int group_list_empty(void)
{
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;
    return (l->head == NULL);
}

static int group_list_enqueue(nk_thread_group *g)
{
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;
    spin_lock(&l->group_list_lock);
    g->group_id = get_next_groupid();

    if (l == NULL) {
        GROUP("GROUP_LIST IS UNINITIALIZED.\n");
        goto bad;
    }

    if (l->head == NULL) {
        l->head = group_node_init(g);
        if (!l->head) {
            goto bad;
        } else {
            l->tail = l->head;
            goto ok;
        }
    }

    group_node *n = l->tail;
    l->tail = group_node_init(g);
    if (!l->tail) {
        goto bad;
    }

    l->tail->prev = n;
    n->next = l->tail;

ok:
    spin_unlock(&l->group_list_lock);
    return 0;

bad:
    spin_unlock(&l->group_list_lock);
    return -1;
}


/*
static void rt_list_map(rt_list *l, void (func)(rt_thread *t, void *priv), void *priv)
{
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;
    rt_node *n = l->head;
    while (n != NULL) {
    func(n->thread,priv);
        n = n->next;
    }
}
*/

static nk_thread_group* group_list_remove(nk_thread_group *g)
{
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;
    spin_lock(&l->group_list_lock);

    group_node *n = l->head;
    while (n != NULL) {
        if (n->group == g) {
            group_node *tmp = n->next;
            nk_thread_group *f;
            if (n->next != NULL) {
                n->next->prev = n->prev;
            } else {
                l->tail = n->prev;
            }

            if (n->prev != NULL) {
                n->prev->next = tmp;
            } else {
                l->head = tmp;
            }

            spin_unlock(&l->group_list_lock);

            n->next = NULL;
            n->prev = NULL;
            f = n->group;
            group_node_deinit(n);

            return f;
        }
        n = n->next;
    }

    spin_unlock(&l->group_list_lock);
    return NULL;
}

uint64_t get_next_groupid(void){
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;
    if (l->tail == NULL){
        return 0;
    } else {
        return l->tail->group->group_id + 1;
    }
}

void
thread_unit_list_enqueue(nk_thread_group* group, thread_unit* new_unit){
    int cpu = new_unit->thread->current_cpu;
    thread_unit* head = group->thread_unit_list[cpu];
    if (head){
        //non empty
        while(head->next){
            head = head->next;
        }
        new_unit->prev = head;
        head->next = new_unit;
    } else {
        //empty
        group->thread_unit_list[cpu] = new_unit;
        new_unit -> prev = NULL;
    }

    return;
}

thread_unit *
thread_unit_list_dequeue(nk_thread_group* group, nk_thread_t* toremove){
    int cpu = toremove->current_cpu;
    thread_unit* head = group->thread_unit_list[cpu];
    while (head){
        if (head->thread == toremove){
            thread_unit* hprev = head->prev;
            thread_unit* hnext = head->next;

            if (hprev){
                hprev->next = hnext;
            } else {
                //it is the head
                group->thread_unit_list[cpu] = hnext;
            }

            if (hnext){
                hnext->prev = hprev;
            } //it is not the tail

            return head;
        }
        head = head->next;
    }
    GROUP("thread to remove is not found in group thread_unit_list\n");
    return NULL;
}

// search for a thread group by name
struct nk_thread_group *
nk_thread_group_find(char *name){
    parallel_thread_group_list_t * l = &parallel_thread_group_list;;

    spin_lock(&l->group_list_lock);
        GROUP("In group_find\n");
    group_node *n = l->head;
    while (n != NULL) {
            GROUP("%s\n", n->group->group_name);
        if (! (strcmp(n->group->group_name, name) ))  {
            spin_unlock(&l->group_list_lock);
            return n->group;
        }
        n = n->next;
    }
    spin_unlock(&l->group_list_lock);
    return NULL;
}

// current thread joins a group
int
nk_thread_group_join(struct nk_thread_group *group, uint64_t *dur){
    spin_lock(&group->group_lock);
    group_barrier_join(group->group_barrier);
    group->group_size++;
    int id = group->next_id++;
    //add to thread list
    thread_unit* new_thread_unit = malloc(sizeof(thread_unit));
    new_thread_unit->tid = id;
    new_thread_unit->thread = get_cur_thread();
    new_thread_unit->next = NULL;
    new_thread_unit->prev = NULL;

    thread_unit_list_enqueue(group, new_thread_unit);
    GROUP("group_size = %d\n", group->group_size);
    spin_unlock(&group->group_lock);

    group->dur_dump[id] = dur;
    return id;
}

// current thread leaves a group
int
nk_thread_group_leave(struct nk_thread_group *group){
    spin_lock(&group->group_lock);
    group->group_size--;
    //remove from thread list
    thread_unit* toremove = thread_unit_list_dequeue(group, get_cur_thread());
    FREE(toremove);

    spin_unlock(&group->group_lock);
    group_barrier_leave(group->group_barrier);
    return 0;
}

// all threads in the group call to synchronize
int
nk_thread_group_barrier(struct  nk_thread_group *group) {
    GROUP("nk_thread_group_barrier\n");
    return group_barrier_wait(group->group_barrier);
}

// all threads in the group call to select one thread as leader
uint64_t
nk_thread_group_election(struct nk_thread_group *group, uint64_t my_tid) {
    //uint64_t my_tid = group_get_my_tid();
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
nk_thread_group_broadcast(struct nk_thread_group *group, void *message, uint64_t tid, uint64_t src) {
  if(tid != src) {
    //receiver
    int ret = atomic_inc_val(group->msg_count);
    GROUP("msg_count = %d\n", group->msg_count);
    while (group->msg_flag == 0) {
      GROUP("t%d is waiting\n", tid);
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


struct nk_thread_group *
nk_thread_group_create(char *name) {
    nk_thread_group* new_group = (nk_thread_group *) malloc(sizeof(nk_thread_group));
    new_group->group_name = name;
    new_group->group_leader = -1;

    //clear thread_unit_list
    memset(new_group->thread_unit_list, 0, MAX_CPU_NUM * sizeof(thread_unit*));

    new_group->group_size = 0;
    new_group->init_fail = 0;
    new_group->next_id = 0;
    new_group->group_barrier = (nk_barrier_t *) malloc(sizeof(nk_barrier_t));

    new_group->message = NULL;
    new_group->msg_flag = 0;
    new_group->msg_count = 0;

    new_group->group_constraints = malloc(sizeof(struct nk_sched_constraints));
    new_group->changing_constraint = 0;
    new_group->changing_fail = 0;
    new_group->changing_count = 0;
    new_group->change_cons_wait_q = nk_thread_queue_create();
    new_group->sleep_count = 0;

    spinlock_init(&new_group->group_lock);
    //new_group->group_id = get_next_groupid(); group id is assigned in group_list_enqueue()

     if (group_list_enqueue(new_group)){ //will acquire list_lock in group_list_queue
         GROUP("group_list enqueue failed\n");
         FREE(new_group);
         return NULL;
     }

     if(group_barrier_init(new_group->group_barrier)) {
        GROUP("group_barrier_init failed\n");
        FREE(new_group->group_barrier);
        FREE(new_group);
        return NULL;
     } else {
        return new_group;
     }
}

// delete a group (should be empty)
int
nk_thread_group_delete(struct nk_thread_group *group) {
    if (group == group_list_remove(group)){
        GROUP("delete group node succeeded!\n");
        FREE(group);
        return 0;
    } else {
        GROUP("delete group node failed!\n");
        return -1;
    }
}
extern struct nk_sched_constraints* get_rt_constraint(struct nk_thread *t);
int group_roll_back_constraint();

int
group_set_constraint(struct nk_thread_group *group, struct nk_sched_constraints *constraints) {
  group->group_constraints = constraints;
  return 0;
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

int
group_change_constraint(struct nk_thread_group *group, int tid) {
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
      if (nk_sched_thread_change_constraints(group->group_constraints) != 0) {
        //if fail, set the failure flag
        atomic_cmpswap(group->changing_fail, 0, 1);
      }
    }
    nk_thread_group_barrier(group);
  } else {
    //check if there is failure, if so, don't do local change constraint
    if (group->changing_fail == 0) {
      if (nk_sched_thread_change_constraints(group->group_constraints) != 0) {
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
  nk_thread_group *dst = nk_thread_group_find((char*) in);
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
    dst->group_constraints->type = APERIODIC;
    dst->group_constraints->interrupt_priority_class = 0x1;
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
  nk_sched_thread_change_constraints(dst->group_constraints);
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
    nk_thread_group_init();
    char group_name[20];
    sprintf(group_name, "helloworld!");
    nk_thread_group * new_group = nk_thread_group_create(group_name);
    if (new_group != NULL) {
        GROUP("group_create succeeded\n");
    } else {
        GROUP("group_create failed\n");
        return -1;
    }

    nk_thread_group * ret = nk_thread_group_find(group_name);
    if (ret != new_group){
        GROUP("result from group_create does not match group_find!\n");
    }
    // launch a few aperiodic threads (testers), i.e. regular threads
    // each join the group
    int i;
    for (i = 0; i < TESTER_NUM; ++i){
        if (launch_tester(group_name, i)){
            GROUP("starting tester failed\n");
        }
    }

    return 0;
}

//Barrier
static inline void
bspin_lock (volatile int * lock)
{
        while (__sync_lock_test_and_set(lock, 1));
}

static inline void
bspin_unlock (volatile int * lock)
{
        __sync_lock_release(lock);
}

int
group_barrier_init (nk_barrier_t * barrier)
{
    DEBUG_PRINT("Initializing group barrier, group barrier at %p, count=%u\n", (void*)barrier, 0);

    memset(barrier, 0, sizeof(nk_barrier_t));
    barrier->lock = 0;
    barrier->notify = 0;
    barrier->init_count = 0;
    barrier->remaining  = 0;

    return 0;
}

int
group_barrier_wait (nk_barrier_t * barrier)
{
    int res = 0;

    bspin_lock(&barrier->lock);
    DEBUG_PRINT("Thread (%p) entering barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        DEBUG_PRINT("Thread (%p): notify\n", (void*)get_cur_thread());
    } else {
        DEBUG_PRINT("Thread (%p): remaining count = %d\n", (void*)get_cur_thread(), barrier->remaining);
        bspin_unlock(&barrier->lock);
        BARRIER_WHILE(barrier->notify != 1);
    }

    if (atomic_inc_val(barrier->remaining) == barrier->init_count) {
      atomic_cmpswap(barrier->notify, 1, 0); //last thread reset notify
      DEBUG_PRINT("Thread (%p): reset notify\n", (void*)get_cur_thread());
      bspin_unlock(&barrier->lock);
    }

    DEBUG_PRINT("Thread (%p) exiting barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    return res;
}

int
group_barrier_join (nk_barrier_t * barrier)
{
    int res = 0;

    bspin_lock(&barrier->lock);
    DEBUG_PRINT("Thread (%p) joining barrier \n", (void*)get_cur_thread());
    atomic_inc(barrier->init_count);
    atomic_inc(barrier->remaining);
    bspin_unlock(&barrier->lock);

    return res;
}

int
group_barrier_leave (nk_barrier_t * barrier)
{
    int res = 0;

    DEBUG_PRINT("Thread (%p) leaving barrier (%p)\n", (void*)get_cur_thread(), (void*)barrier);

    bspin_lock(&barrier->lock);

    atomic_dec(barrier->init_count);

    if (--barrier->remaining == 0) {
        res = NK_BARRIER_LAST;
        atomic_cmpswap(barrier->notify, 0, 1); //last thread set notify
        DEBUG_PRINT("Thread (%p): notify\n", (void*)get_cur_thread());
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
