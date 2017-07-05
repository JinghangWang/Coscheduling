#include <nautilus/burner.h>

static void burner(void *in, void **out)
{
    uint64_t start, end, dur, total;
    total = 0;
    struct burner_args *a = (struct burner_args *)in;

    nk_thread_name(get_cur_thread(),a->name);

    if (nk_bind_vc(get_cur_thread(), a->vc)) {
	ERROR_PRINT("Cannot bind virtual console for burner %s\n",a->name);
	return;
    }

    //nk_vc_printf("%s (tid %llu) attempting to promote itself\n", a->name, get_cur_thread()->tid);
#if 1
    if (nk_sched_thread_change_constraints(&a->constraints)) {
	nk_vc_printf("%s (tid %llu) rejected - exiting\n", a->name, get_cur_thread()->tid);
  free(in);
	return;
    }
#endif

    //nk_vc_printf("%s (tid %llu) promotion complete - spinning for %lu ns\n", a->name, get_cur_thread()->tid, a->size_ns);
    while(1) {
    	start = nk_sched_get_realtime();
    	udelay(100);
    	end = nk_sched_get_realtime();
    	dur = end - start;
      //total += dur;
    	//nk_vc_printf("%s (tid %llu) start=%llu, end=%llu left=%llu\n",a->name,get_cur_thread()->tid, start, end,a->size_ns);
    	if (dur >= a->size_ns) {
    	    //nk_vc_printf("%s (tid %llu) done - exiting, total runtime = %llu\n",a->name,get_cur_thread()->tid, total);

          struct rt_stats* stats = malloc(sizeof(struct rt_stats));
          nk_sched_rt_stats(stats);
    	    nk_vc_printf("%s (tid %llu) exiting period %llu ns, slice %llu ns ", a->name, get_cur_thread()->tid, stats->period, stats->slice);
          nk_vc_printf("arrival count %llu, resched count %llu, switchin count %llu, miss count %llu, total miss time %llu ns\n",
                      stats->arrival_num, stats->resched_num, stats->switchin_num, stats->miss_num, stats->miss_time);
          free(stats);
          free(in);
          //nk_vc_printf("exiting...\n");
    	    //nk_thread_exit(0);
          return;
    	} else {
    	    a->size_ns -= dur;
          //nk_vc_printf("%s size set into burner in ns %llu\n",a->name, a->size_ns);
    	}
    }
}

int launch_aperiodic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t priority)
{
    nk_thread_id_t tid;
    struct burner_args *a;

    a = malloc(sizeof(struct burner_args));
    if (!a) {
	return -1;
    }

    strncpy(a->name,name,MAX_CMD); a->name[MAX_CMD-1]=0;
    a->vc = get_cur_thread()->vc;
    a->size_ns = size_ns;
    a->constraints.type=APERIODIC;
    a->constraints.interrupt_priority_class = (uint8_t) tpr;
    a->constraints.aperiodic.priority=priority;

    if (nk_thread_start(burner, (void*)a , NULL, 1, PAGE_SIZE_4KB, &tid, -1)) {
	free(a);
	return -1;
    } else {
	return 0;
    }
}

int launch_sporadic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t phase, uint64_t size, uint64_t deadline, uint64_t aperiodic_priority)
{
    nk_thread_id_t tid;
    struct burner_args *a;

    a = malloc(sizeof(struct burner_args));
    if (!a) {
	return -1;
    }

    strncpy(a->name,name,MAX_CMD); a->name[MAX_CMD-1]=0;
    a->vc = get_cur_thread()->vc;
    a->size_ns = size_ns;
    a->constraints.type=SPORADIC;
    a->constraints.interrupt_priority_class = (uint8_t) tpr;
    a->constraints.sporadic.phase = phase;
    a->constraints.sporadic.size = size;
    a->constraints.sporadic.deadline = deadline;
    a->constraints.sporadic.aperiodic_priority = aperiodic_priority;

    if (nk_thread_start(burner, (void*)a , NULL, 1, PAGE_SIZE_4KB, &tid, -1)) {
	free(a);
	return -1;
    } else {
	return 0;
    }
}

int launch_periodic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t phase, uint64_t period, uint64_t slice, uint64_t cpu)
{
    nk_thread_id_t tid;
    struct burner_args *a;

    a = malloc(sizeof(struct burner_args));
    if (!a) {
	return -1;
    }

    strncpy(a->name,name,MAX_CMD); a->name[MAX_CMD-1]=0;
    a->vc = get_cur_thread()->vc;
    a->size_ns = size_ns;
    a->constraints.type=PERIODIC;
    a->constraints.interrupt_priority_class = (uint8_t) tpr;
    a->constraints.periodic.phase = phase;
    a->constraints.periodic.period = period;
    a->constraints.periodic.slice = slice;

    if (nk_thread_start(burner, (void*)a , NULL, 1, PAGE_SIZE_4KB, &tid, cpu)) {
    //bind cpu 1 for testing
	free(a);
	return -1;
    } else {
  //nk_vc_printf("before join \n");
  //wait until burner has finished
  nk_join(tid, NULL);
  //nk_vc_printf("join works\n");
	return 0;
    }
}
