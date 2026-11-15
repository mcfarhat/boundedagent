#!/bin/sh
# Runs ON the QNX target. Brings the stack up, exercises the guard, tears down.
# SAFE=cpu mask for safety cluster, AI=cpu mask for the AI cluster.
SAFE=${SAFE:-0x2}; AI=${AI:-0x1}
cd /data/ba
slay -f ctrld guardd sensord loadgen agentd_stub 2>/dev/null
./sensord > sensord.log 2>&1 &
./ctrld -C $SAFE -p 60 > ctrld.log 2>&1 &
sleep 1
./guardd -C $SAFE -p 55 > guardd.log 2>&1 &
sleep 1
echo "--- status at boot"; cat /dev/servo/arm0
echo "--- 1. agent proposals, human far (2.0 m)"
./agentd_stub -n 10 -i 100 -C $AI
cat /dev/servo/arm0
echo "--- 2. human steps to 0.8 m (SLOW zone)"
echo 0.8 > /dev/range0; sleep 1
echo "0 30 120" > /dev/servo/arm0; cat /dev/servo/arm0
echo "--- 3. human at 0.3 m (HOLD)"
echo 0.3 > /dev/range0; sleep 1
echo "0 -30 50" > /dev/servo/arm0 || echo "write refused (expected)"
cat /dev/servo/arm0
echo "--- 4a. human leaves (2.0 m): guard returns to RUN but nothing moves until a new request"
echo 2.0 > /dev/range0; sleep 1; cat /dev/servo/arm0
echo "0 20 60" > /dev/servo/arm0 && echo "new request accepted"
echo "--- 4b. sensor gets stuck -> must HOLD"
echo freeze > /dev/range0; sleep 1; cat /dev/servo/arm0
echo "--- 4c. sensor recovers, then the sensor process dies -> must HOLD"
echo 2.0 > /dev/range0; sleep 1; slay sensord; sleep 1
cat /dev/servo/arm0
echo "--- 6. flood the guard"
i=0; while [ $i -lt 120 ]; do echo "1 5 10" > /dev/servo/arm0 2>/dev/null; i=$((i+1)); done
cat /dev/servo/arm0
sleep 1
echo "=== ctrld log"; tail -4 ctrld.log
echo "=== guardd log (tail)"; tail -12 guardd.log
slay -f ctrld guardd sensord loadgen agentd_stub 2>/dev/null
