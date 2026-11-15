/* Real-time helpers: priority + runmask (CPU cluster pinning). */
#ifndef BA_RT_H
#define BA_RT_H
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

/* Pin the calling thread AND threads it creates afterwards to `mask` (bit n = CPU n). */
static inline int ba_pin(unsigned mask) {
    if (!mask) return 0;
    unsigned *rmaskp; int size = RMSK_SIZE(_syspage_ptr->num_cpu);
    unsigned *buf = calloc(1, sizeof(int) + 2 * size * sizeof(unsigned));
    *(int *)buf = size;              /* number of unsigneds per mask, NOT num_cpu */
    rmaskp = buf + 1;                /* runmask, then inherit mask */
    rmaskp[0] = mask;                
    rmaskp[size] = mask;
    int rc = ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, buf);
    free(buf);
    if (rc == -1) fprintf(stderr, "ba_pin(0x%x): %s\n", mask, strerror(errno));
    return rc;
}

static inline int ba_fifo(int prio) {
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc) fprintf(stderr, "ba_fifo(%d): %s\n", prio, strerror(rc));
    return rc;
}
#endif
