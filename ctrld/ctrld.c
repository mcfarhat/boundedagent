/*
 * ctrld: the hard-real-time control loop.
 *  - 1 kHz periodic timer delivered as a pulse; never blocks on anything else.
 *  - Accepts setpoints ONLY as messages from guardd (it is the sole client by design).
 *  - Integrates a simulated plant (slew-rate-limited joints) or, later, drives the PCA9685.
 *  - Measures its own lateness every tick: this is the number the demo is about.
 *
 * usage: ctrld [-p prio] [-C runmask] [-r period_us] [-d deadline_us] [-q]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include "../common/ba.h"
#include "../common/rt.h"

typedef union {
    struct _pulse   pulse;
    uint16_t        type;
    ba_setpoint_t   sp;
    ba_stats_req_t  sreq;
    char            raw[256];
} ctrl_msg_t;

static float pos[BA_NJOINTS], tgt[BA_NJOINTS], vel[BA_NJOINTS];

int main(int argc, char **argv) {
    int prio = 60, opt, quiet = 0;
    unsigned mask = 0, period_us = 1000, deadline_us = 500;
    while ((opt = getopt(argc, argv, "p:C:r:d:q")) != -1) switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'C': mask = strtoul(optarg, NULL, 0); break;
        case 'r': period_us = atoi(optarg); break;
        case 'd': deadline_us = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default: fprintf(stderr, "usage: ctrld [-p prio] [-C runmask] [-r period_us] [-d deadline_us] [-q]\n"); return 1;
    }
    ba_pin(mask);
    ba_fifo(prio);

    name_attach_t *att = name_attach(NULL, BA_CTRLD_NAME, 0);
    if (!att) { perror("name_attach"); return 1; }
    int coid = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, prio, BA_PULSE_TICK, 0);   /* tick arrives at loop priority */
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) { perror("timer_create"); return 1; }
    struct itimerspec its = {
        .it_value    = { 0, period_us * 1000 },
        .it_interval = { 0, period_us * 1000 },
    };
    uint64_t prev_ns = 0;
    timer_settime(tid, 0, &its, NULL);

    /* stats window */
    uint64_t ticks = 0, misses = 0, sum_late = 0, win_ticks = 0;
    uint32_t max_late = 0;
    uint64_t total_ticks = 0, total_misses = 0;
    const float dt = period_us / 1e6f;

    if (!quiet) printf("ctrld: up, prio %d, runmask 0x%x, period %uus, deadline %uus\n",
                       prio, mask, period_us, deadline_us);
    fflush(stdout);

    for (;;) {
        ctrl_msg_t m;
        int rcvid = MsgReceive(att->chid, &m, sizeof m, NULL);
        if (rcvid == -1) continue;

        if (rcvid == 0) {                                   /* pulse */
            if (m.pulse.code == BA_PULSE_TICK) {
                total_ticks++;
                /* jitter = |actual tick interval - period|; the first tick only sets the baseline */
                uint64_t now = ba_now_ns();
                if (!prev_ns) { prev_ns = now; continue; }
                int64_t dev_ns = (int64_t)(now - prev_ns) - (int64_t)period_us * 1000;
                prev_ns = now;
                if (dev_ns < 0) dev_ns = -dev_ns;
                uint32_t late_us = (uint32_t)(dev_ns / 1000);
                if (late_us > deadline_us) { misses++; total_misses++; }
                if (late_us > max_late) max_late = late_us;
                sum_late += late_us; win_ticks++; ticks++;

                for (int j = 0; j < BA_NJOINTS; j++) {      /* simulated plant */
                    float err = tgt[j] - pos[j], step = vel[j] * dt;
                    if (fabsf(err) <= step) pos[j] = tgt[j];
                    else pos[j] += (err > 0 ? step : -step);
                }
                if (!quiet && ticks % (1000000 / period_us) == 0) {
                    printf("ctrld: cpu%u  jitter avg %4lluus max %5uus  misses %llu/%llu  j0 %.1f->%.1f  j1 %.1f->%.1f\n",
                           SchedGetCpuNum(),
                           (unsigned long long)(win_ticks ? sum_late / win_ticks : 0), max_late,
                           (unsigned long long)misses, (unsigned long long)win_ticks,
                           pos[0], tgt[0], pos[1], tgt[1]);
                    fflush(stdout);
                    win_ticks = sum_late = 0; max_late = 0; misses = 0;
                }
            } else if (m.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(m.pulse.scoid);
            }
            continue;
        }

        if (m.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (m.type > _IO_BASE && m.type <= _IO_MAX) { MsgError(rcvid, ENOSYS); continue; }

        switch (m.type) {
        case BA_MSG_SETPOINT:
            if (m.sp.joint >= BA_NJOINTS || !isfinite(m.sp.pos_deg) || !isfinite(m.sp.vel_dps) || m.sp.vel_dps < 0) {
                MsgError(rcvid, EINVAL); break;
            }
            if (m.sp.vel_dps == 0) tgt[m.sp.joint] = pos[m.sp.joint];   /* vel 0 = HOLD: freeze where we are */
            else tgt[m.sp.joint] = m.sp.pos_deg;
            vel[m.sp.joint] = m.sp.vel_dps;
            MsgReply(rcvid, EOK, &pos[m.sp.joint], sizeof(float));
            break;
        case BA_MSG_STATS: {
            ba_stats_t s = {0};
            s.ticks = total_ticks; s.misses = total_misses;
            s.max_late_us = max_late;
            s.avg_late_us = win_ticks ? (uint32_t)(sum_late / win_ticks) : 0;
            memcpy(s.pos_deg, pos, sizeof pos); memcpy(s.tgt_deg, tgt, sizeof tgt);
            s.cpu = SchedGetCpuNum();
            MsgReply(rcvid, EOK, &s, sizeof s);
            break;
        }
        default:
            MsgError(rcvid, ENOSYS);
        }
    }
}
