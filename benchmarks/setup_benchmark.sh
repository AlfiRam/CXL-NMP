#!/bin/bash

################################################################################
# Setup Script for NMP Memory Stride Benchmark
#
# This script:
# 1. Builds the m5ops library (if needed)
# 2. Compiles the memory benchmark with m5ops support
# 3. Mounts the disk image
# 4. Copies the benchmark to the disk image
# 5. Unmounts the disk image
################################################################################

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISK_IMAGE="../fs_files/parsec.img"
MOUNT_POINT="/tmp/parsec_mount"
BENCHMARK_NAME="memory_stride_access"

echo "================================================================================"
echo "NMP Memory Stride Benchmark Setup"
echo "================================================================================"
echo ""

# Step 1: Build the benchmark
echo "[1/5] Building benchmark..."
cd "$SCRIPT_DIR"
make clean
make memory_stride_access

if [ ! -f "$BENCHMARK_NAME" ]; then
    echo "ERROR: Failed to build benchmark"
    exit 1
fi

echo "✓ Benchmark built successfully (with m5ops linked)"
echo ""

# Step 2: Create mount point
echo "[2/5] Creating mount point..."
mkdir -p "$MOUNT_POINT"
echo "✓ Mount point ready: $MOUNT_POINT"
echo ""

# Step 3: Mount disk image
echo "[3/5] Mounting disk image..."
if mountpoint -q "$MOUNT_POINT"; then
    echo "  Disk already mounted, unmounting first..."
    sudo umount "$MOUNT_POINT"
fi

sudo mount -o loop,offset=$((2048*512)) "$DISK_IMAGE" "$MOUNT_POINT"

if ! mountpoint -q "$MOUNT_POINT"; then
    echo "ERROR: Failed to mount disk image"
    exit 1
fi

echo "✓ Disk image mounted"
echo ""

# Step 4: Copy benchmark to disk image
echo "[4/5] Installing benchmark to disk image..."
sudo cp "$BENCHMARK_NAME" "$MOUNT_POINT/home/cxl_benchmark/$BENCHMARK_NAME"
sudo chmod +x "$MOUNT_POINT/home/cxl_benchmark/$BENCHMARK_NAME"

# Verify installation
if [ -f "$MOUNT_POINT/home/cxl_benchmark/$BENCHMARK_NAME" ]; then
    echo "✓ Benchmark installed:"
    ls -lh "$MOUNT_POINT/home/cxl_benchmark/$BENCHMARK_NAME"
else
    echo "ERROR: Failed to install benchmark"
    sudo umount "$MOUNT_POINT"
    exit 1
fi
echo ""

# Step 5: Show available benchmarks
echo "[5/5] Available benchmarks in disk image:"
ls -lh "$MOUNT_POINT/home/cxl_benchmark/" | grep -E "^-.*x.*"
echo ""

# Unmount
echo "Unmounting disk image..."
sudo umount "$MOUNT_POINT"
echo "✓ Disk image unmounted"
echo ""

echo "================================================================================"
echo "Setup Complete!"
echo "================================================================================"
echo ""
echo "Your benchmark is now installed in the disk image at:"
echo "  /home/cxl_benchmark/$BENCHMARK_NAME"
echo ""
echo "Next steps:"
echo "  1. Run the NMP experiment:"
echo "     cd .."
echo "     ./run_nmp_experiments.sh $BENCHMARK_NAME TIMING ASIC"
echo ""
echo "  2. Check the results:"
echo "     less output/nmp_${BENCHMARK_NAME}_TIMING_ASIC/simout.txt"
echo ""
echo "================================================================================"
