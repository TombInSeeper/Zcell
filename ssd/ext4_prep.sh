#!/bin/bash
set -x

dev=/dev/nvme0n1

umount /mnt/ext4
nvme format -s 1 -b 4096 ${dev}
mkfs.ext4 ${dev}
mount ${dev} /mnt/ext4
rm -rf /run/perf/ext4
mkdir -p /run/perf/ext4