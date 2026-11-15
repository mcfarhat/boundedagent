/* BoundedAgent shared definitions. */
#ifndef BA_H
#define BA_H
#include <stdint.h>
#include <sys/iomsg.h>
#include <time.h>

#define BA_CTRLD_NAME   "ba_ctrld"          /* name_attach() name of the control loop */
#define BA_SERVO_PATH   "/dev/servo/arm0"   /* guardd: the ONLY path to the actuator  */
#define BA_RANGE_PATH   "/dev/range0"       /* sensord: human distance in metres      */
#define BA_NJOINTS      2

/* Message types to ctrld (must be > _IO_MAX for name_attach servers). */
enum { BA_MSG_SETPOINT = _IO_MAX + 1, BA_MSG_STATS };
enum { BA_PULSE_TICK = _PULSE_CODE_MINAVAIL };

typedef struct {
    uint16_t type;          /* BA_MSG_SETPOINT */
    uint16_t joint;
    float    pos_deg;       /* target position */
    float    vel_dps;       /* max slew rate toward target, deg/s (0 = hold) */
    uint32_t seq;
} ba_setpoint_t;

typedef struct {
    uint16_t type;          /* BA_MSG_STATS */
    uint16_t reset;         /* 1 = reset window after reading */
} ba_stats_req_t;

typedef struct {
    uint64_t ticks;
    uint64_t misses;        /* ticks later than the deadline */
    uint32_t max_late_us;   /* worst lateness in window */
    uint32_t avg_late_us;
    float    pos_deg[BA_NJOINTS];
    float    tgt_deg[BA_NJOINTS];
    uint32_t cpu;           /* CPU the loop last ran on */
} ba_stats_t;

static inline uint64_t ba_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#endif
