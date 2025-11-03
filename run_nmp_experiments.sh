#!/bin/bash

################################################################################
# NMP (Near-Memory Processing) Experiment Runner
#
# This script runs benchmarks on both Host CPU (Core 0) and NMP CPU (Core 1)
# to demonstrate the latency advantage of co-locating computation with CXL memory.
#
# Expected Results:
#   - Core 0 (Host):  ~382 ns per access (baseline)
#   - Core 1 (NMP):   ~230-305 ns per access (target)
#   - Speedup:        1.25-1.7×
#
# Usage:
#   ./run_nmp_experiments.sh [BENCHMARK] [CPU_TYPE] [DEVICE_TYPE]
#
# Arguments:
#   BENCHMARK    - Benchmark to run (default: pointer_chase)
#                  Options: pointer_chase, lmbench_cxl.sh, stream_cxl.sh, etc.
#   CPU_TYPE     - CPU model (default: TIMING)
#                  Options: TIMING, O3
#   DEVICE_TYPE  - CXL device type (default: ASIC)
#                  Options: ASIC, FPGA
#
# Examples:
#   ./run_nmp_experiments.sh pointer_chase TIMING ASIC
#   ./run_nmp_experiments.sh lmbench_cxl.sh O3 ASIC
################################################################################

# Default parameters
BENCHMARK="${1:-pointer_chase}"
CPU_TYPE="${2:-TIMING}"
DEVICE_TYPE="${3:-ASIC}"

# Convert device type to boolean flag
if [ "$DEVICE_TYPE" = "ASIC" ]; then
    IS_ASIC="True"
else
    IS_ASIC="False"
fi

# Output directory
OUTPUT_DIR="output/nmp_${BENCHMARK}_${CPU_TYPE}_${DEVICE_TYPE}"

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
    echo "ERROR: NMP config script not found at $CONFIG_SCRIPT"
    exit 1
fi

# Print experiment configuration
echo "================================================================================"
echo "NMP Experiment Configuration"
echo "================================================================================"
echo "Benchmark:     $BENCHMARK"
echo "CPU Type:      $CPU_TYPE"
echo "Device Type:   $DEVICE_TYPE (is_asic=$IS_ASIC)"
echo "Output Dir:    $OUTPUT_DIR"
echo "Config:        $CONFIG_SCRIPT"
echo ""
echo "Experiment Design:"
echo "  - Core 0 (Host CPU):  Accesses CXL via CXLBridge + CXLMemory (~77ns overhead)"
echo "  - Core 1 (NMP CPU):   Direct access to CXL Memory Bus (bypass overhead)"
echo "  - Routing:            Port-based (transparent to software)"
echo "  - Execution:          taskset automatically binds to each core"
echo ""
echo "Expected Results:"
echo "  - Core 0 latency: ~382 ns (measured baseline)"
echo "  - Core 1 latency: ~230-305 ns (target)"
echo "  - Speedup:        1.25-1.7×"
echo "================================================================================"
echo ""

# Run the simulation
echo "Starting simulation..."
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
    echo "Simulation completed successfully!"
    echo "================================================================================"
    echo ""
    echo "Output directory: $OUTPUT_DIR"
    echo ""
    echo "Next Steps:"
    echo "  1. Check simulation output:"
    echo "     less $OUTPUT_DIR/simout.txt"
    echo ""
    echo "  2. Analyze statistics:"
    echo "     # Core 0 (Host) memory accesses"
    echo "     grep 'board.processor.switch.core.0' $OUTPUT_DIR/stats.txt | grep -E 'numCycles|numReads|numWrites'"
    echo ""
    echo "     # Core 1 (NMP) memory accesses"
    echo "     grep 'board.processor.switch.core.1' $OUTPUT_DIR/stats.txt | grep -E 'numCycles|numReads|numWrites'"
    echo ""
    echo "     # CXL Bridge activity (should show Core 0 only)"
    echo "     grep 'board.bridge' $OUTPUT_DIR/stats.txt | grep -E 'numReads|numWrites'"
    echo ""
    echo "     # NMP Bridge activity (should show Core 1 only)"
    echo "     grep 'board.nmp_bridge' $OUTPUT_DIR/stats.txt | grep -E 'numReads|numWrites'"
    echo ""
    echo "  3. Verify routing paths:"
    echo "     grep -A 5 'board.bridge' $OUTPUT_DIR/config.ini | grep ranges"
    echo "     grep -A 5 'board.nmp_bridge' $OUTPUT_DIR/config.ini | grep ranges"
    echo ""
    echo "  4. Compare latencies:"
    echo "     # Extract benchmark results from simout.txt"
    echo "     grep -A 10 'Running Host CPU' $OUTPUT_DIR/simout.txt"
    echo "     grep -A 10 'Running NMP CPU' $OUTPUT_DIR/simout.txt"
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
