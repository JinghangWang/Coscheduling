#include <nautilus/spinlock.h>
#include <nautilus/queue.h>
#include <nautilus/scheduler.h>
#include <nautilus/thread.h>
#include <nautilus/barrier.h>

#define APIC_GROUP_JOIN_VEC    0xf6

int group_test(int num_members);
void nk_thread_queue_sleep_count(nk_thread_queue_t *wq, int *count);

int group_barrier_init (nk_barrier_t * barrier);
int group_barrier_wait (nk_barrier_t * barrier);
int group_barrier_join (nk_barrier_t * barrier);
int group_barrier_leave (nk_barrier_t * barrier);
