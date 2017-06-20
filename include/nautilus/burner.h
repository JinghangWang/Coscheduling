#include <nautilus/nautilus.h>
#include <nautilus/cpuid.h>
#include <nautilus/vc.h>
#include <nautilus/shell.h>

int launch_periodic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t phase, uint64_t period, uint64_t slice, uint64_t cpu);
int launch_sporadic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t phase, uint64_t size, uint64_t deadline, uint64_t aperiodic_priority);
int launch_aperiodic_burner(char *name, uint64_t size_ns, uint32_t tpr, uint64_t priority);
