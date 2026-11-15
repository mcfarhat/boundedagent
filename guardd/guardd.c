/*
 * guardd: the runtime-assurance gate (Simplex "safety controller" side).
 * Owns /dev/servo/arm0. It is the ONLY process that talks to ctrld, so the AI's
 * only route to the actuator is a write() that this process judges.
 *
 *   write "<joint> <pos_deg> <vel_dps>\n"
 *      -> accepted (maybe velocity-clamped)  : write() succeeds
 *      -> outside envelope / malformed       : EPERM / EINVAL
 *      -> HOLD (human too close, stale sensor): EBUSY
 *      -> request flood                      : EAGAIN
 *   read -> one-line status for observability.
 *
 * Speed & separation monitoring (after ISO 10218 / ISO/TS 15066 SSM):
 *   d >= D_SLOW         RUN   full speed limit
 *   D_HOLD <= d < D_SLOW SLOW  limit scaled linearly down to SLOW_MIN_FRAC
 *   d < D_HOLD or stale  HOLD  all joints frozen, motion requests refused
 * Leaving HOLD never resumes motion by itself: a fresh request is required.
 *
 * usage: guardd [-p prio] [-C runmask]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "../common/ba.h"
#include "../common/rt.h"

/* ---- policy (one place, so the codelab can point at it) ---- */
static const float POS_MIN[BA_NJOINTS] = { -90.f, -45.f };
static const float POS_MAX[BA_NJOINTS] = {  90.f,  45.f };
static const float VEL_MAX = 120.f;           /* deg/s, unrestricted zone */
static const float D_SLOW = 1.00f, D_HOLD = 0.50f, SLOW_MIN_FRAC = 0.2f;
static const unsigned SENSOR_STALE_MS = 200;
static const unsigned MONITOR_HZ = 50;
static const unsigned RATE_PER_S = 50;        /* token bucket for requests */

typedef enum { ST_RUN, ST_SLOW, ST_HOLD } state_t;
static const char *ST_NAME[] = { "RUN", "SLOW", "HOLD" };

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static state_t  state = ST_HOLD;              /* boot safe: HOLD until the sensor proves otherwise */
static float    dist = -1.f;
static const char *hold_reason = "boot";
static uint64_t n_acc, n_clamp, n_rej, n_hold;
static char     last[96] = "none";
static uint32_t seq;
static int      ctrl_coid = -1;
static double   tokens = RATE_PER_S; static uint64_t tok_t;

static int send_sp(int j, float pos, float vel) {
    ba_setpoint_t sp = { BA_MSG_SETPOINT, (uint16_t)j, pos, vel, ++seq };
    float cur;
    return MsgSend(ctrl_coid, &sp, sizeof sp, &cur, sizeof cur);
}

static float vel_limit(void) {           /* call with mtx held */
    if (state == ST_RUN)  return VEL_MAX;
    if (state == ST_SLOW) {
        float f = (dist - D_HOLD) / (D_SLOW - D_HOLD);
        if (f < SLOW_MIN_FRAC) f = SLOW_MIN_FRAC;
        return VEL_MAX * f;
    }
    return 0.f;
}

static void audit(const char *verdict, int j, float p, float v, const char *why) {
    snprintf(last, sizeof last, "%s j%d %.1f@%.0f %s", verdict, j, p, v, why);
    printf("guardd: [%s d=%.2f] %s\n", ST_NAME[state], dist, last);
    fflush(stdout);
}

/* ---- monitor thread: sensor -> state machine, fail-safe on any doubt ---- */
static void *monitor(void *arg) {
    (void)arg;
    int fd = -1; char buf[64];
    for (;;) {
        float d = -1.f; unsigned age = ~0u; const char *why = NULL;
        if (fd < 0) fd = open(BA_RANGE_PATH, O_RDONLY);
        if (fd < 0) why = "no sensor";
        else {
            ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
            if (n <= 0) { why = "sensor read failed"; close(fd); fd = -1; }
            else { buf[n] = 0; if (sscanf(buf, "%f %u", &d, &age) != 2) why = "sensor garbage"; }
        }
        if (!why && age > SENSOR_STALE_MS) why = "sensor stale";

        state_t ns = why ? ST_HOLD : d < D_HOLD ? ST_HOLD : d < D_SLOW ? ST_SLOW : ST_RUN;
        pthread_mutex_lock(&mtx);
        dist = d;
        if (ns != state) {
            state_t old = state; state = ns;
            hold_reason = ns == ST_HOLD ? (why ? why : "human within hold distance") : "";
            printf("guardd: STATE %s -> %s (d=%.2f%s%s)\n", ST_NAME[old], ST_NAME[ns], d,
                   ns == ST_HOLD ? ", " : "", ns == ST_HOLD ? hold_reason : "");
            fflush(stdout);
            if (ns == ST_HOLD)
                for (int j = 0; j < BA_NJOINTS; j++) send_sp(j, 0, 0);   /* vel 0 = freeze where you are */
            else if (ns == ST_SLOW && old == ST_RUN)
                /* motion in flight was approved at the RUN limit: freeze it, so any further
                   motion must be a new request judged against the SLOW limit */
                for (int j = 0; j < BA_NJOINTS; j++) send_sp(j, 0, 0);
        }
        pthread_mutex_unlock(&mtx);
        usleep(1000000 / MONITOR_HZ);
    }
    return NULL;
}

/* ---- resource manager ---- */
static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    char buf[96];
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    size_t n = _IO_WRITE_GET_NBYTES(msg);
    if (n == 0 || n >= sizeof buf) return n ? E2BIG : EINVAL;
    if (resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1) return errno;
    buf[n] = 0;

    int j; float p, v; char extra;
    int rc = EOK;
    pthread_mutex_lock(&mtx);
    /* token bucket */
    uint64_t now = ba_now_ns();
    tokens += (now - tok_t) / 1e9 * RATE_PER_S; tok_t = now;
    if (tokens > RATE_PER_S) tokens = RATE_PER_S;

    if (tokens < 1) { n_rej++; audit("REJECT", -1, 0, 0, "rate limit"); rc = EAGAIN; goto out; }
    tokens -= 1;
    if (sscanf(buf, "%d %f %f %c", &j, &p, &v, &extra) != 3 || !isfinite(p) || !isfinite(v)) {
        n_rej++; audit("REJECT", -1, 0, 0, "malformed"); rc = EINVAL; goto out;
    }
    if (j < 0 || j >= BA_NJOINTS) { n_rej++; audit("REJECT", j, p, v, "no such joint"); rc = EPERM; goto out; }
    if (p < POS_MIN[j] || p > POS_MAX[j]) { n_rej++; audit("REJECT", j, p, v, "outside envelope"); rc = EPERM; goto out; }
    if (v <= 0) { n_rej++; audit("REJECT", j, p, v, "non-positive velocity"); rc = EINVAL; goto out; }
    if (state == ST_HOLD) { n_hold++; audit("HOLD", j, p, v, hold_reason); rc = EBUSY; goto out; }

    float lim = vel_limit();
    const char *verdict = "ACCEPT";
    if (v > lim) { v = lim; verdict = "CLAMP"; n_clamp++; } else n_acc++;
    if (send_sp(j, p, v) == -1) { audit("FAULT", j, p, v, "ctrld unreachable"); rc = EIO; goto out; }
    audit(verdict, j, p, v, state == ST_SLOW ? "slow zone" : "");
out:
    pthread_mutex_unlock(&mtx);
    if (rc != EOK) return rc;
    _IO_SET_WRITE_NBYTES(ctp, n);
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    static char buf[256];
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    pthread_mutex_lock(&mtx);
    int len = snprintf(buf, sizeof buf,
        "state=%s dist=%.2f vlim=%.0f accepted=%llu clamped=%llu rejected=%llu held=%llu last=\"%s\"\n",
        ST_NAME[state], dist, vel_limit(), (unsigned long long)n_acc, (unsigned long long)n_clamp,
        (unsigned long long)n_rej, (unsigned long long)n_hold, last);
    pthread_mutex_unlock(&mtx);
    if (ocb->offset >= len) { _IO_SET_READ_NBYTES(ctp, 0); return _RESMGR_NPARTS(0); }
    size_t n = len - ocb->offset; if (n > msg->i.nbytes) n = msg->i.nbytes;
    SETIOV(ctp->iov, buf + ocb->offset, n);
    ocb->offset += n;
    _IO_SET_READ_NBYTES(ctp, n);
    return _RESMGR_NPARTS(1);
}

int main(int argc, char **argv) {
    int prio = 55, opt; unsigned mask = 0;
    while ((opt = getopt(argc, argv, "p:C:")) != -1) switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'C': mask = strtoul(optarg, NULL, 0); break;
        default: fprintf(stderr, "usage: guardd [-p prio] [-C runmask]\n"); return 1;
    }
    ba_pin(mask);           /* pins this thread and (inherit mask) the monitor thread */
    ba_fifo(prio);
    tok_t = ba_now_ns();

    for (int i = 0; i < 50 && (ctrl_coid = name_open(BA_CTRLD_NAME, 0)) == -1; i++) usleep(100000);
    if (ctrl_coid == -1) { perror("name_open ctrld"); return 1; }

    pthread_attr_t pa; pthread_attr_init(&pa);
    pthread_attr_setinheritsched(&pa, PTHREAD_INHERIT_SCHED);
    pthread_t th; pthread_create(&th, &pa, monitor, NULL);

    dispatch_t *dpp = dispatch_create();     /* channel created WITHOUT _NTO_CHF_INHERIT_RUNMASK:
                                                AI clients can't drag guard work onto their cores */
    resmgr_attr_t rattr; memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1; rattr.msg_max_size = 512;
    resmgr_connect_funcs_t cf; resmgr_io_funcs_t iof;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = io_read; iof.write = io_write;
    static iofunc_attr_t attr;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, BA_SERVO_PATH, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        perror("resmgr_attach"); return 1;
    }
    printf("guardd: %s up, prio %d, runmask 0x%x (boot state HOLD until sensor is fresh)\n",
           BA_SERVO_PATH, prio, mask);
    fflush(stdout);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) { if ((ctp = dispatch_block(ctp))) dispatch_handler(ctp); }
}
