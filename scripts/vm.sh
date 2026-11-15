#!/bin/bash
# Helpers for the QEMU QNX VM (see ~/qnxvm/run.sh). usage: vm.sh deploy | ssh '<cmd>'
O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
PW=${QNX_VM_PW:-root}
here=$(cd "$(dirname "$0")/.." && pwd)
case "$1" in
  deploy) sshpass -p "$PW" ssh -p 2222 "${O[@]}" root@127.0.0.1 'mkdir -p /data/ba' &&
          sshpass -p "$PW" scp -P 2222 "${O[@]}" "$here"/build/x86_64/* "$here"/scripts/*.qnx.sh root@127.0.0.1:/data/ba/ ;;
  ssh)    shift; sshpass -p "$PW" ssh -p 2222 "${O[@]}" root@127.0.0.1 "$@" ;;
  *) echo "usage: $0 deploy|ssh <cmd>"; exit 1 ;;
esac
