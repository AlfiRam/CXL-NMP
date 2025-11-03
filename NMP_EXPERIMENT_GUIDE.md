# NMP Experiment Guide - Separate Host and NMP Runs

## Overview

The NMP experiments have been configured to run **separately** with **independent output directories** for cleaner statistics collection and easier comparison.

### Experiment Setup

| Experiment | Cores | NMP | Routing Path | Output Dir | Expected Latency |
|------------|-------|-----|--------------|------------|------------------|
| **Host Baseline** | 1 (Core 0) | Disabled | L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus | `m5out_host` | ~382 ns |
| **NMP Test** | 2 (Core 1 active) | Enabled | L3 → MemBus → NMPBridge → CXL_Mem_Bus (DIRECT) | `m5out_nmp` | ~230-305 ns |

**Expected Speedup**: 1.25-1.7× (NMP vs Host)

---

## Quick Start

### 1. Run Host Baseline (Core 0)

```bash
./run_host_baseline.sh memory_stride_access TIMING ASIC
```

This will:
- Boot OS with 1 core
- Run benchmark on Core 0 (normal CXL path through CXLBridge)
- Output statistics to `m5out_host/stats.txt`
- Measure baseline latency (~382ns expected)

### 2. Run NMP Experiment (Core 1)

```bash
./run_nmp_experiment.sh memory_stride_access TIMING ASIC
```

This will:
- Boot OS with 2 cores
- Run benchmark ONLY on Core 1 using `taskset -c 1` (direct CXL path)
- Output statistics to `m5out_nmp/stats.txt`
- Measure NMP latency (~230-305ns expected)

### 3. Compare Results

```bash
# Host baseline cycles
grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt

# NMP cycles
grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt

# Calculate speedup
# Speedup = Host_Cycles / NMP_Cycles
```

---

## File Structure

### Configuration Files

1. **`configs/example/gem5_library/x86-cxl-host-run.py`**
   - Single-core configuration (Core 0 only)
   - NMP disabled (normal CXL path)
   - No taskset needed
   - Used by `run_host_baseline.sh`

2. **`configs/example/gem5_library/x86-cxl-nmp-run.py`**
   - Two-core configuration (Core 0 + Core 1)
   - NMP enabled (port-based routing)
   - Uses `taskset -c 1` to run only on Core 1
   - Used by `run_nmp_experiment.sh`

### Run Scripts

1. **`run_host_baseline.sh`** - Runs host baseline experiment
2. **`run_nmp_experiment.sh`** - Runs NMP experiment

### Output Directories

1. **`m5out_host/`** - Host baseline results
   - `stats.txt` - Host statistics
   - `simout.txt` - Console output
   - `config.ini` - Configuration details

2. **`m5out_nmp/`** - NMP experiment results
   - `stats.txt` - NMP statistics
   - `simout.txt` - Console output
   - `config.ini` - Configuration details

---

## Detailed Usage

### Host Baseline Experiment

```bash
# Basic usage
./run_host_baseline.sh

# With custom benchmark
./run_host_baseline.sh lmbench_cxl.sh

# With O3 CPU
./run_host_baseline.sh memory_stride_access O3 ASIC

# Monitor progress
tail -f m5out_host/simout.txt
```

**What happens:**
1. gem5 boots Linux with KVM (fast boot)
2. Switches to TIMING/O3 core
3. Runs `/home/cxl_benchmark/memory_stride_access` on Core 0
4. Benchmark calls `m5_reset_stats()` before loop
5. Benchmark calls `m5_dump_stats()` after loop
6. Statistics saved to `m5out_host/stats.txt`

### NMP Experiment

```bash
# Basic usage
./run_nmp_experiment.sh

# With custom benchmark
./run_nmp_experiment.sh lmbench_cxl.sh

# With O3 CPU
./run_nmp_experiment.sh memory_stride_access O3 ASIC

# Monitor progress
tail -f m5out_nmp/simout.txt
```

**What happens:**
1. gem5 boots Linux with KVM on 2 cores
2. Switches to TIMING/O3 cores
3. Runs `taskset -c 1 /home/cxl_benchmark/memory_stride_access`
4. Benchmark executes ONLY on Core 1 (NMP path)
5. Core 0 remains idle during benchmark
6. Statistics saved to `m5out_nmp/stats.txt`

---

## Statistics Analysis

### Key Statistics to Check

#### Host Baseline (`m5out_host/stats.txt`)

```bash
# Total cycles for Core 0
grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt

# Instructions committed
grep 'board.processor.switch.core.0.committedInsts' m5out_host/stats.txt

# CXL Bridge activity (should show reads/writes)
grep 'board.bridge' m5out_host/stats.txt | grep -E 'numReads|numWrites'

# L3 cache misses
grep 'board.cache_hierarchy.l3cache' m5out_host/stats.txt | grep 'overallMisses'
```

#### NMP Experiment (`m5out_nmp/stats.txt`)

```bash
# Total cycles for Core 1
grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt

# Instructions committed
grep 'board.processor.switch.core.1.committedInsts' m5out_nmp/stats.txt

# NMP Bridge activity (should show reads/writes from Core 1)
grep 'board.nmp_bridge' m5out_nmp/stats.txt | grep -E 'numReads|numWrites'

# CXL Bridge activity (should show minimal or no activity since Core 0 is idle)
grep 'board.bridge' m5out_nmp/stats.txt | grep -E 'numReads|numWrites'

# Verify port-based routing (both bridges have same address range)
grep -A 3 'board.bridge]' m5out_nmp/config.ini | grep ranges
grep -A 3 'board.nmp_bridge]' m5out_nmp/config.ini | grep ranges
```

### Calculate Speedup

```bash
# Extract cycle counts
HOST_CYCLES=$(grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt | awk '{print $2}')
NMP_CYCLES=$(grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt | awk '{print $2}')

# Calculate speedup
echo "Host Cycles: $HOST_CYCLES"
echo "NMP Cycles: $NMP_CYCLES"
echo "Speedup: $(echo "scale=2; $HOST_CYCLES / $NMP_CYCLES" | bc)"
```

Expected output:
```
Host Cycles: XXXXXX
NMP Cycles: YYYYYY
Speedup: 1.25-1.70
```

---

## Verification Checklist

### Host Baseline Verification

- [ ] Simulation completed without errors
- [ ] Only 1 core visible in simulation
- [ ] `board.bridge` shows read/write activity
- [ ] `board.processor.switch.core.0` has non-zero cycles
- [ ] Benchmark output shows "Benchmark Complete"

### NMP Experiment Verification

- [ ] Simulation completed without errors
- [ ] 2 cores visible in simulation (via `lscpu` or config.ini)
- [ ] Benchmark ran with `taskset -c 1`
- [ ] `board.nmp_bridge` shows read/write activity
- [ ] `board.bridge` shows minimal/no activity (Core 0 idle)
- [ ] `board.processor.switch.core.1` has non-zero cycles
- [ ] Both bridges have SAME address range in config.ini (port-based routing)
- [ ] Benchmark output shows "Benchmark Complete"

### Routing Path Verification

```bash
# Verify both bridges handle same addresses (0x100000000 range)
grep 'ranges=' m5out_nmp/config.ini | grep -E 'bridge|nmp_bridge' -A 1

# Expected output:
# [board.bridge]
# ranges=...0x100000000:0x2FFFFFFFF...
# [board.nmp_bridge]
# ranges=...0x100000000:0x2FFFFFFFF...
```

This confirms port-based routing (NOT address-based).

---

## Troubleshooting

### Issue: Host simulation shows NMP bridge

**Symptom**: `m5out_host/config.ini` contains `board.nmp_bridge`

**Solution**: Make sure you're using `run_host_baseline.sh` which calls `x86-cxl-host-run.py` (not the NMP config)

### Issue: NMP simulation runs on Core 0

**Symptom**: `board.processor.switch.core.0` shows activity in `m5out_nmp/stats.txt`

**Solution**: Check that `taskset -c 1` is in the command (verify in x86-cxl-nmp-run.py:187)

### Issue: Both simulations take the same time

**Symptom**: Host and NMP cycle counts are similar

**Possible causes**:
1. Benchmark not accessing CXL memory (check address range)
2. NMP routing not enabled (check `enable_nmp=True` in config)
3. Cache hierarchy issue (check L3 misses in stats)

### Issue: Simulation hangs at boot

**Solution**:
1. Check KVM support: `lsmod | grep kvm`
2. Try with `--cpu_type TIMING` instead of `O3`
3. Check available memory on host system

---

## Expected Timeline

- **Host Baseline**: 30 minutes - 2 hours (depending on system)
- **NMP Experiment**: 30 minutes - 2 hours (depending on system)
- **Total**: ~1-4 hours for both experiments

**Note**: Simulations can run overnight if needed.

---

## Next Steps After Experiments Complete

1. **Extract Results**:
   ```bash
   # Create results summary
   echo "=== Host Baseline ===" > results.txt
   grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt >> results.txt
   echo "" >> results.txt
   echo "=== NMP Experiment ===" >> results.txt
   grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt >> results.txt
   ```

2. **Calculate Metrics**:
   - Average latency per access
   - Speedup ratio
   - Memory bandwidth utilization

3. **Generate Graphs**:
   - Latency comparison (Host vs NMP)
   - Cycle count comparison
   - Bridge activity comparison

4. **Prepare for Advisor**:
   - Document speedup achieved
   - Compare with expected 1.25-1.7× target
   - Analyze any deviations from expected results

---

## Quick Reference

```bash
# Run both experiments back-to-back
./run_host_baseline.sh memory_stride_access TIMING ASIC
./run_nmp_experiment.sh memory_stride_access TIMING ASIC

# Compare cycle counts
diff <(grep 'numCycles' m5out_host/stats.txt | head -1) \
     <(grep 'numCycles' m5out_nmp/stats.txt | head -1)

# View benchmark output
grep "=== NMP Memory Stride Benchmark ===" -A 20 m5out_host/simout.txt
grep "=== NMP Memory Stride Benchmark ===" -A 20 m5out_nmp/simout.txt
```

---

## Additional Resources

- **Implementation Details**: `NMP_IMPLEMENTATION_SUMMARY.md`
- **Verification Checklist**: `NMP_VERIFICATION_CHECKLIST.md`
- **Research Context**: `RESEARCH_CONTEXT.md`
- **Benchmark Documentation**: `benchmarks/README.md`
