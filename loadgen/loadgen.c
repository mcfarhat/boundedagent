/* loadgen: CPU hog standing in for a busy local model. usage: loadgen [-t threads] [-C runmask] [-p prio] [-s secs] */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "../common/ba.h"
#include "../common/rt.h"
static volatile int run = 1;
static void *spin(void *a) { (void)a; volatile double x = 1; while (run) x = x * 1.0000001 + 1e-9; return NULL; }
int main(int argc, char **argv) {
    int n = 4, prio = 0, secs = 0, opt; unsigned mask = 0;
    while ((opt = getopt(argc, argv, "t:C:p:s:")) != -1) switch (opt) {
        case 't': n = atoi(optarg); break;
        case 'C': mask = strtoul(optarg, NULL, 0); break;
        case 'p': prio = atoi(optarg); break;
        case 's': secs = atoi(optarg); break;
    }
    ba_pin(mask);
    /* spinners get `prio`; the main (timer) thread sits one level above so it can always stop them */
    pthread_attr_t pa; pthread_attr_init(&pa);
    if (prio) {
        struct sched_param sp = { .sched_priority = prio };
        pthread_attr_setinheritsched(&pa, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&pa, SCHED_FIFO);
        pthread_attr_setschedparam(&pa, &sp);
        ba_fifo(prio + 1);
    }
    printf("loadgen: %d spinning threads, runmask 0x%x, prio %d%s\n", n, mask, prio, prio ? " FIFO" : " (default)");
    fflush(stdout);
    pthread_t t[64]; if (n > 64) n = 64;
    for (int i = 0; i < n; i++) pthread_create(&t[i], &pa, spin, NULL);
    if (secs) { sleep(secs); run = 0; } 
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    return 0;
}
