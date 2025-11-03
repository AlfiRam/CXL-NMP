# NMP Implementation Verification Checklist

## Status: Ready for Testing

### Implementation Summary

All NMP components have been successfully implemented:
- ✅ x86-cxl-nmp-run.py:195 - 2-core processor configuration
- ✅ x86_board.py:186 - NMP bridge with port-based routing
- ✅ nmp_private_l1_private_l2_shared_l3_cache_hierarchy.py:157 - Port-based routing logic
- ✅ run_nmp_experiments.sh - Experiment runner script
- ✅ gem5.opt binary built successfully (733MB)

---

## Pre-Flight Verification Steps

Before running the full simulation, verify the configuration:

### 1. Check Required Files Exist

```bash
# Configuration files
ls -lh configs/example/gem5_library/x86-cxl-nmp-run.py
ls -lh src/python/gem5/components/boards/x86_board.py
ls -lh src/python/gem5/components/cachehierarchies/classic/nmp_private_l1_private_l2_shared_l3_cache_hierarchy.py

# gem5 binary
ls -lh build/X86/gem5.opt

# Experiment script
ls -lh run_nmp_experiments.sh
```

**Expected Output**: All files present

---

### 2. Verify Kernel and Disk Image Paths

The NMP configuration requires these files (specified in x86-cxl-nmp-run.py:161-162):

```python
kernel=KernelResource(local_path='/home/xxx/code/fs_image/vmlinux_20240920')
disk_image=DiskImageResource(local_path='/home/xxx/code/fs_image/parsec.img')
```

**ACTION REQUIRED**: Update these paths to match your system:

```bash
# Check if default paths exist
ls -lh /home/xxx/code/fs_image/vmlinux_20240920
ls -lh /home/xxx/code/fs_image/parsec.img
```

If files don't exist at these paths, you need to either:
- Update the paths in x86-cxl-nmp-run.py to point to your actual kernel and disk image
- Or create symbolic links to the correct locations

---

### 3. Verify Benchmark Exists in Disk Image

The configuration runs benchmarks from `/home/cxl_benchmark/` inside the simulated system:

```bash
# The disk image should contain:
# - /home/cxl_benchmark/pointer_chase
# - /home/cxl_benchmark/lmbench_cxl.sh
# - /home/cxl_benchmark/stream_cxl.sh
# etc.
```

**Note**: These files must exist in the disk image, not on the host system.

---

### 4. Configuration Validation

Key implementation points to verify in config.ini after boot:

```ini
# Both bridges should have SAME address range (0x100000000+)
[board.bridge]
ranges=...0x100000000:0x2FFFFFFFF...

[board.nmp_bridge]
ranges=...0x100000000:0x2FFFFFFFF...

# Both cores should be visible
[board.processor.switch.core.0]
[board.processor.switch.core.1]

# L3 cache shared between cores
[board.cache_hierarchy.l3cache]
```

---

## Quick Boot Test (Recommended Before Full Run)

Test that the system boots with 2 cores:

```bash
# Short boot test (will exit after boot)
timeout 5m build/X86/gem5.opt -d output/nmp_boot_test \
    configs/example/gem5_library/x86-cxl-nmp-run.py \
    --is_asic True \
    --test_cmd pointer_chase \
    --cpu_type TIMING
```

**Expected Output**:
```
[NMP] Near-Memory Processing enabled - Port-based routing:
[NMP]   Address range: 0x100000000 - 0x2FFFFFFFF
[NMP]   Core 0 (Host):  L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus
[NMP]   Core 1 (NMP):   L3 → MemBus → NMP_Bridge → CXL_Mem_Bus (DIRECT)
[NMP]   Bypass savings: ~77ns (CXLBridge 62ns + CXLMemory 15ns)
[NMP]   Routing method: Port-based (transparent to software)
```

Then check the output:
```bash
# Verify 2 cores visible
grep "System booted. Two cores available" output/nmp_boot_test/simout.txt

# Check for errors
grep -i "fatal\|panic\|error" output/nmp_boot_test/simout.txt
```

---

## Full Experiment Run

Once pre-flight checks pass, run the full experiment:

```bash
# Using the experiment script
./run_nmp_experiments.sh pointer_chase TIMING ASIC

# Or manually
build/X86/gem5.opt -d output/nmp_pointer_chase_TIMING_ASIC \
    configs/example/gem5_library/x86-cxl-nmp-run.py \
    --is_asic True \
    --test_cmd pointer_chase \
    --cpu_type TIMING
```

---

## Post-Simulation Verification

After simulation completes, verify the NMP implementation:

### 1. Check Both Cores Executed

```bash
# Should show execution on Core 0
grep "Running Host CPU (Core 0)" output/nmp_*/simout.txt

# Should show execution on Core 1
grep "Running NMP CPU (Core 1)" output/nmp_*/simout.txt
```

### 2. Verify Routing Paths

```bash
# CXL Bridge should show activity ONLY from Core 0
grep "board.bridge" output/nmp_*/stats.txt | grep -E "numReads|numWrites"

# NMP Bridge should show activity ONLY from Core 1
grep "board.nmp_bridge" output/nmp_*/stats.txt | grep -E "numReads|numWrites"
```

### 3. Check Address Ranges (Port-Based Routing Confirmation)

```bash
# Both bridges should have SAME address range
grep -A 3 "board.bridge\]" output/nmp_*/config.ini | grep ranges
grep -A 3 "board.nmp_bridge\]" output/nmp_*/config.ini | grep ranges

# Expected: Both show ranges=...0x100000000:0x2FFFFFFFF
```

### 4. Compare Latencies

```bash
# Extract per-core statistics
# Core 0 (Host)
grep "board.processor.switch.core.0" output/nmp_*/stats.txt | grep -E "numCycles|committedInsts"

# Core 1 (NMP)
grep "board.processor.switch.core.1" output/nmp_*/stats.txt | grep -E "numCycles|committedInsts"

# Calculate average latency per access:
# latency = (cycles * clock_period) / num_accesses
```

---

## Expected Results

### Performance Targets

| Configuration | Expected Latency | Speedup |
|---------------|------------------|---------|
| Core 0 (Host) | ~382 ns | 1.0× (baseline) |
| Core 1 (NMP) | ~230-305 ns | 1.25-1.7× |

### Success Criteria

- ✅ System boots with 2 cores visible to Linux
- ✅ Both cores execute the benchmark (via taskset)
- ✅ config.ini shows both bridges with same address range
- ✅ CXLBridge statistics show Core 0 traffic only
- ✅ NMPBridge statistics show Core 1 traffic only
- ✅ Core 1 latency < Core 0 latency
- ✅ Speedup in range 1.25-1.7×

---

## Troubleshooting

### Issue: Kernel/Disk Image Not Found

**Error**: `fatal: Unable to stat file /home/xxx/code/fs_image/vmlinux_20240920`

**Solution**: Update paths in x86-cxl-nmp-run.py:161-162 to match your system

### Issue: Benchmark Not Found in Simulation

**Error**: `sh: /home/cxl_benchmark/pointer_chase: not found`

**Solution**: Verify benchmark is compiled and exists in the disk image at the correct path

### Issue: Only One Core Visible

**Symptom**: `lscpu` shows 1 CPU instead of 2

**Check**:
```bash
# Verify processor configuration
grep "num_cores" configs/example/gem5_library/x86-cxl-nmp-run.py
# Should show: num_cores=2
```

### Issue: No NMP Bridge Activity

**Symptom**: board.nmp_bridge statistics show 0 reads/writes

**Check**:
1. Verify Core 1 actually executed the benchmark
2. Check that the benchmark accessed CXL memory (0x100000000 range)
3. Verify cache hierarchy routing logic in nmp_private_l1_private_l2_shared_l3_cache_hierarchy.py:157

### Issue: Simulation Hangs

**Symptom**: Simulation doesn't progress past boot

**Solution**:
1. Check KVM support: `lsmod | grep kvm`
2. Try with --cpu_type TIMING instead of O3
3. Check for sufficient memory on host

---

## Next Steps After Successful Test

1. **Collect Statistics**: Run multiple benchmarks (pointer_chase, lmbench, stream)
2. **Analyze Results**: Compare latencies across different workloads
3. **Vary Parameters**: Test with O3 CPU, different memory configurations
4. **Document Findings**: Update NMP_IMPLEMENTATION_SUMMARY.md with measured results
5. **Prepare for Advisor**: Create graphs and summary of speedup results

---

## Quick Reference: Key File Locations

```
NMP Implementation Files:
├── configs/example/gem5_library/x86-cxl-nmp-run.py (Main config)
├── src/python/gem5/components/boards/x86_board.py (NMP bridge setup)
├── src/python/gem5/components/cachehierarchies/classic/
│   └── nmp_private_l1_private_l2_shared_l3_cache_hierarchy.py (Routing logic)
├── run_nmp_experiments.sh (Experiment runner)
├── NMP_IMPLEMENTATION_SUMMARY.md (Architecture documentation)
└── NMP_VERIFICATION_CHECKLIST.md (This file)

Simulation Outputs:
└── output/nmp_[benchmark]_[cpu]_[device]/
    ├── simout.txt (Console output, boot messages, benchmark results)
    ├── stats.txt (Performance statistics)
    └── config.ini (Actual configuration used)
```

---

## Contact & Documentation

- Research Context: `RESEARCH_CONTEXT.md`
- Implementation Details: `NMP_IMPLEMENTATION_SUMMARY.md`
- gem5 Documentation: https://www.gem5.org/documentation/
- CXL-DMSim Paper: `CXL-DMSim_A_Full-System_CXL_Disaggregated_Memory_Simulator.pdf`

---

**Status**: Implementation complete, ready for testing
**Last Updated**: 2025-10-31
