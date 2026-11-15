# BoundedAgent

A runtime-assurance layer for LLM-driven robots on QNX 8.0. The AI can only *propose* motion, through a
file (`/dev/servo/arm0`). A guard process judges every proposal against a safety envelope and the
distance to the nearest human (speed and separation monitoring). The control loop runs on its own core,
where no AI workload can reach it.

Status: week 1 skeleton, running in QEMU (x86_64). It also builds for the Pi 5 (aarch64le) with no warnings.

## Processes

| Process | Role | Runs on |
|---|---|---|
| `ctrld` | 1 kHz control loop (timer pulse), simulated plant, measures its own tick jitter. Takes setpoints only as messages. | SAFETY core, FIFO 60 |
| `guardd` | Resource manager `/dev/servo/arm0`. Envelope, velocity clamp, RUN/SLOW/HOLD state machine, rate limit, audit log. The only client of ctrld. | SAFETY core, FIFO 55 |
| `sensord` | Resource manager `/dev/range0`, human distance. Sim mode: `echo 0.4 > /dev/range0`, `echo freeze > /dev/range0`. | any |
| `agentd_stub` | Stand-in for the LLM agent. Proposes setpoints, every 5th one deliberately unsafe. | AI cores |
| `loadgen` | CPU hog standing in for a busy local model. | AI cores |

## Safety rules (guardd)
- Position outside the per-joint envelope: `EPERM`. Malformed or non-positive velocity: `EINVAL`.
- Velocity above the current limit: clamped and accepted (logged as `CLAMP`).
- Human at 1.0 m or more: RUN, full limit. Between 0.5 and 1.0 m: SLOW, limit scaled down to 20%. Under 0.5 m: HOLD.
- HOLD freezes all joints. Requests during HOLD get `EBUSY`.
- A missing sensor, a stale sample (over 200 ms), unreadable output or a dead sensor process all mean HOLD. Boot state is HOLD.
- Leaving HOLD (or RUN to SLOW) never resumes motion. A fresh request has to be judged again.
- Token bucket of 50 requests/s. A flood gets `EAGAIN`.
- guardd's channel does not set `_NTO_CHF_INHERIT_RUNMASK`, so AI clients can't pull guard work onto AI cores.

## Isolation experiment (`scripts/isolation.qnx.sh`), QEMU TCG, 2 vCPU, 2026-11-15
The AI load is 2 spinning threads at FIFO priority 63, which is above the control loop's 60.

| Run | Control loop |
|---|---|
| A) no load | jitter avg 21-53 us, 0-12 misses per 1000 ticks |
| B) load unpinned | **stalled for 6,002,030 us (the whole load window)**, then a burst replay of stale ticks |
| C) load pinned to the AI core | jitter avg 23-40 us, 0-8 misses per 1000 ticks, no stall |

QEMU on this host is software emulation (no KVM), so the absolute numbers are noise. The published
numbers come from the Pi 5. The contrast between A, B and C is the point.

## Build and run
    source ~/qnx800/qnxsdp-env.sh
    make                      # x86_64 for QEMU
    make ARCH=aarch64le       # Raspberry Pi 5
    scripts/qemu/mkvm.sh      # once: build the QNX 8.0 QEMU image into ~/qnxvm
    ~/qnxvm/run.sh            # boot it headless (no KVM needed); ssh root@127.0.0.1 -p 2222
    scripts/vm.sh deploy
    scripts/vm.sh ssh 'sh /data/ba/smoke.qnx.sh'      # guard behaviour
    scripts/vm.sh ssh 'sh /data/ba/isolation.qnx.sh'  # the A/B/C experiment

## TODO (next)
- ctrld: detect timer overruns and skip missed ticks instead of replaying them; emit a deadline-miss pulse to guardd, which goes to HOLD.
- Freshness/deadline on proposals (stale intent is rejected); per-request max duration.
- recorder: slog2 + incident card on every HOLD/REJECT; stats stream for the dashboard.
- agentd: real LLM agent (tool = write to /dev/servo/arm0), Python or C + HTTP.
- Pi 5: named clusters via `startup -c safety:0x8 -c ai:0x3 -c obs:0x4`; PCA9685 (i2c-dwc-rpi5) and range sensor drivers.

## Licence
Apache-2.0. Built for the QNX Global Hackathon 2026 (QNX SDP 8.0, non-commercial licence).
