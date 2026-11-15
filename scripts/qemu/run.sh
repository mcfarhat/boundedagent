#!/bin/bash
# Boot the QNX 8.0 x86_64 QEMU image headless (TCG, no KVM on this host).
# Serial console -> output/qemu.out ; SSH -> 127.0.0.1:2222 ; QEMU monitor -> output/mon.sock
cd "$(dirname "$0")"
exec qemu-system-x86_64 -cpu max -smp 2 -m 1G \
  -kernel output/ifs.bin \
  -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 \
  -display none -serial file:output/qemu.out -monitor unix:output/mon.sock,server,nowait \
  -pidfile output/qemu.pid -daemonize
