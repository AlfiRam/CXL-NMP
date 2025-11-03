#!/bin/bash

################################################################################
# Host Baseline Experiment Runner
#
# This script runs the HOST BASELINE configuration with a single core accessing
# CXL memory through the normal path (CXLBridge + CXLMemory device).
#
# Configuration:
#   - 1 Core (Core 0)
#   - Normal CXL path: L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus
#   - Expected latency: ~382ns per access
#   - Output directory: m5out_host
#
# Usage:
#   ./run_host_baseline.sh [BENCHMARK] [CPU_TYPE] [DEVICE_TYPE]
#
# Arguments:
#   BENCHMARK    - Benchmark to run (default: memory_stride_access)
#   CPU_TYPE     - CPU model (default: TIMING)
#   DEVICE_TYPE  - CXL device type (default: ASIC)
#
# Examples:
#   ./run_host_baseline.sh memory_stride_access TIMING ASIC
#   ./run_host_baseline.sh lmbench_cxl.sh O3 ASIC
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
OUTPUT_DIR="m5out_host"

# gem5 binary
GEM5_BIN="build/X86/gem5.opt"

# Configuration script
CONFIG_SCRIPT="configs/example/gem5_library/x86-cxl-host-run.py"

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
echo "HOST BASELINE Experiment"
echo "================================================================================"
echo "Benchmark:     $BENCHMARK"
echo "CPU Type:      $CPU_TYPE"
echo "Device Type:   $DEVICE_TYPE (is_asic=$IS_ASIC)"
echo "Output Dir:    $OUTPUT_DIR"
echo "Config:        $CONFIG_SCRIPT"
echo ""
echo "Configuration:"
echo "  - Cores:       1 (Core 0 only)"
echo "  - NMP:         DISABLED"
echo "  - Path:        L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus"
echo "  - Overhead:    ~77ns (CXLBridge 62ns + CXLMemory 15ns)"
echo ""
echo "Expected Result: ~382ns latency per access (baseline)"
echo "================================================================================"
echo ""

# Run the simulation
echo "Starting host baseline simulation..."
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
    echo "Host Baseline Simulation Complete!"
    echo "================================================================================"
    echo ""
    echo "Output directory: $OUTPUT_DIR"
    echo ""
    echo "Next Steps:"
    echo "  1. Check simulation output:"
    echo "     less $OUTPUT_DIR/simout.txt"
    echo ""
    echo "  2. Analyze statistics:"
    echo "     # Core 0 (Host) performance"
    echo "     grep 'board.processor.switch.core' $OUTPUT_DIR/stats.txt | grep -E 'numCycles|committedInsts'"
    echo ""
    echo "     # CXL Bridge activity"
    echo "     grep 'board.bridge' $OUTPUT_DIR/stats.txt | grep -E 'numReads|numWrites'"
    echo ""
    echo "  3. Run NMP experiment for comparison:"
    echo "     ./run_nmp_experiment.sh $BENCHMARK $CPU_TYPE $DEVICE_TYPE"
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
