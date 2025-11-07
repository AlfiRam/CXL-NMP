#!/bin/bash
# build_and_install.sh - Compile and install benchmark

set -e

BENCHMARK_NAME="memory_stride_access"
DISK_IMAGE="../fs_files/parsec.img"
MOUNT_POINT="/tmp/parsec_mount"
TARGET_DIR="/home/cxl_benchmark"

echo "=== Building ${BENCHMARK_NAME} ==="
make clean
make

echo ""
echo "=== Installing ${BENCHMARK_NAME} into gem5 disk image ==="

sudo mkdir -p ${MOUNT_POINT}
sudo mount -o loop,offset=$((2048*512)) ${DISK_IMAGE} ${MOUNT_POINT}
sudo cp ${BENCHMARK_NAME} ${MOUNT_POINT}${TARGET_DIR}/
ls -lh ${MOUNT_POINT}${TARGET_DIR}/${BENCHMARK_NAME}
sudo umount ${MOUNT_POINT}

echo "✓ Build and install complete!"
