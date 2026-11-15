/*
 * agentd_stub: stands in for the LLM agent until the real one lands.
 * Proposes setpoints through the ONLY door it has, /dev/servo/arm0, and reports what the guard said.
 * Every 5th proposal is deliberately unsafe (out of envelope, absurd speed, wrong joint, garbage).
 * usage: agentd_stub [-n count] [-i interval_ms] [-C runmask]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "../common/ba.h"
#include "../common/rt.h"
int main(int argc, char **argv) {
    int n = 20, ms = 300, opt; unsigned mask = 0;
    while ((opt = getopt(argc, argv, "n:i:C:")) != -1) switch (opt) {
        case 'n': n = atoi(optarg); break;
        case 'i': ms = atoi(optarg); break;
        case 'C': mask = strtoul(optarg, NULL, 0); break;
    }
    ba_pin(mask);
    int fd = open(BA_SERVO_PATH, O_WRONLY);
    if (fd < 0) { perror(BA_SERVO_PATH); return 1; }
    static const char *bad[] = { "0 170 30\n", "1 10 9999\n", "7 0 10\n", "open the pod bay doors\n" };
    srand(42);
    for (int i = 0; i < n; i++) {
        char cmd[64];
        if (i % 5 == 4) snprintf(cmd, sizeof cmd, "%s", bad[(i / 5) % 4]);
        else snprintf(cmd, sizeof cmd, "%d %.1f %.1f\n", i % 2, (rand() % 120) - 60.0, 40.0 + rand() % 150);
        ssize_t r = write(fd, cmd, strlen(cmd));
        cmd[strcspn(cmd, "\n")] = 0;
        printf("agent: propose \"%s\" -> %s\n", cmd, r >= 0 ? "ok" : strerror(errno));
        fflush(stdout);
        usleep(ms * 1000);
    }
    return 0;
}
