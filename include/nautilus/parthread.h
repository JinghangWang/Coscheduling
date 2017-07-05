#include <nautilus/spinlock.h>
#include <nautilus/queue.h>
#include <nautilus/scheduler.h>
#include <nautilus/thread.h>
#include <nautilus/barrier.h>

#define APIC_GROUP_JOIN_VEC    0xf6

// creating a thread group is done as easily as making a name
struct nk_thread_group *nk_thread_group_create(char *name);

// search for a thread group by name
struct nk_thread_group *nk_thread_group_find(char *name);

// current thread joins a group
int                     nk_thread_group_join(struct nk_thread_group *group, uint64_t* dur);

// current thread leaves a group
int                     nk_thread_group_leave(struct nk_thread_group *group);

// all threads in the group call to synchronize
int                     nk_thread_group_barrier(struct nk_thread_group *group);

// all threads in the group call to select one thread as leader
//struct nk_thread       *nk_thread_group_election(struct nk_thread_group *group);
uint64_t                nk_thread_group_election(struct nk_thread_group *group, uint64_t my_tid);//failure modes, list of threads

// maybe...
// broadcast a message to all members of the thread group
static int              nk_thread_group_broadcast(struct nk_thread_group *group, void *message, uint64_t tid, uint64_t src);

// delete a group (should be empty)
int                     nk_thread_group_delete(struct nk_thread_group *group);

// init/deinit of module
int nk_thread_group_init(void);
int nk_thread_group_deinit(void);
