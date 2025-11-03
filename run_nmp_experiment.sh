#!/bin/bash

################################################################################
# NMP Experiment Runner
#
# This script runs the NMP configuration with Core 1 accessing CXL memory
# through the DIRECT path (bypassing CXLBridge and CXLMemory device).
#
# Configuration:
#   - 2 Cores (Core 0 idle, Core 1 active with taskset)
#   - Direct CXL path: L3 → MemBus → NMPBridge → CXL_Mem_Bus
#   - Expected latency: ~230-305ns per access (1.25-1.7x faster than host)
#   - Output directory: m5out_nmp
#
# Usage:
#   ./run_nmp_experiment.sh [BENCHMARK] [CPU_TYPE] [DEVICE_TYPE]
#
# Arguments:
#   BENCHMARK    - Benchmark to run (default: memory_stride_access)
#   CPU_TYPE     - CPU model (default: TIMING)
#   DEVICE_TYPE  - CXL device type (default: ASIC)
#
# Examples:
#   ./run_nmp_experiment.sh memory_stride_access TIMING ASIC
#   ./run_nmp_experiment.sh lmbench_cxl.sh O3 ASIC
################################################################################

# Default parameters
BENCHMARK="${1:-memory_stride_access}"
CPU_TYPE="${2:-TIMING}"
DEVICE_TYPE="${3:-ASIC}"

# Convert device type to boolean flag
if [ "$DEVICE_TYPE" = "ASIC" ]; then
    IS_ASIC="True"
else
    IS_ASIC="False"
fi

# Output directory
OUTPUT_DIR="m5out_nmp"

# gem5 binary
GEM5_BIN="build/X86/gem5.opt"

# Configuration script
CONFIG_SCRIPT="configs/example/gem5_library/x86-cxl-nmp-run.py"

# Check if gem5 is built
if [ ! -f "$GEM5_BIN" ]; then
    echo "ERROR: gem5 binary not found at $GEM5_BIN"
    echo "Please build gem5 first: scons build/X86/gem5.opt -j16"
    exit 1
fi

# Check if config script exists
if [ ! -f "$CONFIG_SCRIPT" ]; then
    echo "ERROR: Config script not found at $CONFIG_SCRIPT"
    exit 1
fi

# Print experiment configuration
echo "================================================================================"
echo "NMP (Near-Memory Processing) Experiment"
echo "================================================================================"
echo "Benchmark:     $BENCHMARK"
echo "CPU Type:      $CPU_TYPE"
echo "Device Type:   $DEVICE_TYPE (is_asic=$IS_ASIC)"
echo "Output Dir:    $OUTPUT_DIR"
echo "Config:        $CONFIG_SCRIPT"
echo ""
echo "Configuration:"
echo "  - Cores:       2 (Core 0 idle, Core 1 active)"
echo "  - NMP:         ENABLED (Port-based routing)"
echo "  - Path:        L3 → MemBus → NMPBridge → CXL_Mem_Bus (DIRECT)"
echo "  - Bypass:      ~77ns (skips CXLBridge + CXLMemory device)"
echo ""
echo "Expected Result: ~230-305ns latency (1.25-1.7x speedup vs host)"
echo "================================================================================"
echo ""

# Run the simulation
echo "Starting NMP experiment..."
echo "Command: $GEM5_BIN -d $OUTPUT_DIR $CONFIG_SCRIPT --is_asic $IS_ASIC --test_cmd $BENCHMARK --cpu_type $CPU_TYPE"
echo ""

$GEM5_BIN -d "$OUTPUT_DIR" "$CONFIG_SCRIPT" \
    --is_asic "$IS_ASIC" \
    --test_cmd "$BENCHMARK" \
    --cpu_type "$CPU_TYPE"

# Check simulation exit status
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ]; then
    echo ""
    echo "================================================================================"
    echo "NMP Experiment Complete!"
    echo "================================================================================"
    echo ""
    echo "Output directory: $OUTPUT_DIR"
    echo ""
    echo "Next Steps:"
    echo "  1. Check simulation output:"
    echo "     less $OUTPUT_DIR/simout.txt"
    echo ""
    echo "  2. Analyze statistics:"
    echo "     # Core 1 (NMP) performance"
    echo "     grep 'board.processor.switch.core.1' $OUTPUT_DIR/stats.txt | grep -E 'numCycles|committedInsts'"
    echo ""
    echo "     # NMP Bridge activity (should show Core 1 traffic)"
    echo "     grep 'board.nmp_bridge' $OUTPUT_DIR/stats.txt | grep -E 'numReads|numWrites'"
    echo ""
    echo "     # Verify routing (both bridges should have same address range)"
    echo "     grep -A 3 'board.bridge]' $OUTPUT_DIR/config.ini | grep ranges"
    echo "     grep -A 3 'board.nmp_bridge]' $OUTPUT_DIR/config.ini | grep ranges"
    echo ""
    echo "  3. Compare with host baseline:"
    echo "     # Compare cycle counts"
    echo "     echo '=== Host Baseline (Core 0) ==='"
    echo "     grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt"
    echo "     echo '=== NMP (Core 1) ==='"
    echo "     grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt"
    echo ""
    echo "================================================================================"
else
    echo ""
    echo "================================================================================"
    echo "ERROR: Simulation failed with exit code $EXIT_CODE"
    echo "================================================================================"
    echo ""
    echo "Check the output for errors:"
    echo "  less $OUTPUT_DIR/simout.txt"
    echo ""
    exit $EXIT_CODE
fi
