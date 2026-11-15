#!/bin/sh
# Runs ON the QNX target. The core claim of the demo, as an experiment:
#   an AI workload that saturates the CPU at a HIGHER priority than the control loop
#   A) no load   B) load unpinned (shares the control core)   C) load pinned to the AI cluster
SAFE=${SAFE:-0x2}; AI=${AI:-0x1}; SECS=${SECS:-6}; NT=${NT:-2}
cd /data/ba
slay -f ctrld loadgen 2>/dev/null
./ctrld -C $SAFE -p 60 > iso_ctrld.log 2>&1 &
sleep 2
run() {  # $1 label, rest = loadgen args ("" = no load)
  label=$1; shift
  start=$(wc -l < iso_ctrld.log)
  if [ "$1" = "none" ]; then sleep $SECS; else ./loadgen -s $SECS "$@" > /dev/null 2>&1; fi
  sleep 1
  echo "== $label"
  tail -n +$((start+1)) iso_ctrld.log
}
run "A) no AI load"                                   none
run "B) AI load UNPINNED, FIFO prio 63 > ctrld 60"     -t $NT -p 63
run "C) AI load PINNED to AI core, FIFO prio 63"       -t $NT -p 63 -C $AI
slay -f ctrld loadgen 2>/dev/null
