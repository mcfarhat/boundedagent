/*
 * sensord: human-distance sensor as a resource manager at /dev/range0.
 *   read  -> "<metres> <age_ms>\n"
 * Sim mode (default until the Pi): the distance is scripted or set by writing a number:
 *   echo 0.4 > /dev/range0      # a human steps in
 *   echo freeze > /dev/range0   # sensor gets stuck: samples stop being fresh
 * Hardware mode (-H, Pi 5): VL53L0X over I2C or HC-SR04 on GPIO (TODO week 3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include "../common/ba.h"

static float    dist_m = 2.0f;
static uint64_t dist_t;
static int      scripted = 0;   /* -s: slow sinusoidal walk 0.3..2.0 m, 20 s period */
static int      frozen = 0;     /* "freeze" written: simulate a stuck sensor (sample age keeps growing) */
static uint64_t t_start;

static float current(void) {
    if (scripted) {
        double t = (ba_now_ns() - t_start) / 1e9;
        dist_m = (float)(1.15 + 0.85 * cos(2 * M_PI * t / 20.0));
    }
    if (!frozen) dist_t = ba_now_ns();   /* a live sim sensor produces a fresh sample on every read */
    return dist_m;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    static char buf[64];
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    int xt = msg->i.xtype & _IO_XTYPE_MASK;      /* pread() arrives as XTYPE_OFFSET: fine, we ignore offsets */
    if (xt != _IO_XTYPE_NONE && xt != _IO_XTYPE_OFFSET) return ENOSYS;
    float d = current();
    int len = snprintf(buf, sizeof buf, "%.3f %llu\n", d,
                       (unsigned long long)((ba_now_ns() - dist_t) / 1000000));
    /* stateless device: every read returns a fresh sample, ignore offset */
    size_t n = (size_t)len < msg->i.nbytes ? (size_t)len : msg->i.nbytes;
    _IO_SET_READ_NBYTES(ctp, n);
    SETIOV(ctp->iov, buf, n);
    return _RESMGR_NPARTS(1);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    char buf[32];
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    size_t n = _IO_WRITE_GET_NBYTES(msg);
    if (n >= sizeof buf) return E2BIG;
    if (resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1) return errno;
    buf[n] = 0;
    if (!strncmp(buf, "freeze", 6)) { frozen = 1; _IO_SET_WRITE_NBYTES(ctp, n); return EOK; }
    char *end; float v = strtof(buf, &end);
    if (end == buf || !isfinite(v) || v < 0 || v > 10) return EINVAL;
    scripted = 0; frozen = 0; dist_m = v; dist_t = ba_now_ns();
    _IO_SET_WRITE_NBYTES(ctp, n);
    return EOK;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "s")) != -1) if (opt == 's') scripted = 1;
    t_start = dist_t = ba_now_ns();

    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr; memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1; rattr.msg_max_size = 512;
    resmgr_connect_funcs_t cf; resmgr_io_funcs_t iof;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = io_read; iof.write = io_write;
    static iofunc_attr_t attr;
    iofunc_attr_init(&attr, S_IFCHR | 0664, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, BA_RANGE_PATH, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        perror("resmgr_attach"); return 1;
    }
    printf("sensord: %s up (%s)\n", BA_RANGE_PATH, scripted ? "scripted walk" : "sim, settable");
    fflush(stdout);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) { if ((ctp = dispatch_block(ctp))) dispatch_handler(ctp); }
}
