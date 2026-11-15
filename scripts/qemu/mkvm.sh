#!/bin/bash
# Build the QNX 8.0 x86_64 QEMU image with mkqnximage. Needs `source ~/qnx800/qnxsdp-env.sh`.
# usage: scripts/qemu/mkvm.sh [vm_dir]   (default ~/qnxvm)
set -e
VM=${1:-$HOME/qnxvm}
mkdir -p "$VM" && cd "$VM"
mkqnximage --type=qemu --arch=x86_64 --ssh-ident=none --build
cp "$(dirname "$(readlink -f "$0")")/run.sh" "$VM/run.sh"
echo "VM built in $VM. Boot it with $VM/run.sh, then: ssh -p 2222 root@127.0.0.1 (default password root)"
