# Next Steps for NMP Experiment

## Current Status ✅

All code fixes and compilation completed successfully:

1. **✅ Fixed Addr formatting error** - Converted Addr objects to int in x86_board.py print statements
2. **✅ Rebuilt gem5** - Successfully rebuilt with all NMP changes
3. **✅ Compiled benchmark** - memory_stride_access with 128MB + mmap() support
   - Location: `/home/malfiram/CXL-DMSim/benchmarks/memory_stride_access`
   - Size: 893K (static binary)
   - Features: Environment variable support (NMP_CORE), mmap() with specific addresses

## What's Needed: Install Benchmark to Disk Image 🔧

The benchmark needs to be installed to the disk image before running experiments.

### Option 1: Use the Automated Setup Script (RECOMMENDED)

```bash
cd /home/malfiram/CXL-DMSim/benchmarks
sudo ./setup_benchmark.sh
```

This script will:
- Build the benchmark
- Mount the disk image
- Copy benchmark to `/home/cxl_benchmark/memory_stride_access`
- Unmount and clean up

### Option 2: Manual Installation

```bash
cd /home/malfiram/CXL-DMSim/benchmarks

# Mount disk image
sudo mkdir -p /tmp/parsec_mount
sudo mount -o loop,offset=$((2048*512)) ../fs_files/parsec.img /tmp/parsec_mount

# Copy benchmark
sudo cp memory_stride_access /tmp/parsec_mount/home/cxl_benchmark/
sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access

# Verify
ls -lh /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access

# Unmount
sudo umount /tmp/parsec_mount
```

## After Installing: Run Experiments 🚀

### 1. Run NMP Experiment (Core 1, Direct Path)

```bash
cd /home/malfiram/CXL-DMSim
./run_nmp_experiment.sh memory_stride_access TIMING ASIC
```

**Expected behavior:**
- Boots Linux with 2 cores
- Runs benchmark ONLY on Core 1 using `taskset -c 1`
- Uses NMP_CORE=1 environment variable → accesses 0x300000000 address range
- Bypasses CXLBridge via NMP direct path
- Output: `m5out_nmp/stats.txt`

### 2. Run Host Baseline (Core 0, Normal Path)

```bash
cd /home/malfiram/CXL-DMSim
./run_host_baseline.sh memory_stride_access TIMING ASIC
```

**Expected behavior:**
- Boots Linux with 1 core
- Runs benchmark on Core 0
- Uses NMP_CORE=0 environment variable → accesses 0x100000000 address range
- Goes through normal CXL path (CXLBridge + CXLMemory device)
- Output: `m5out_host/stats.txt`

## Expected Results 🎯

### Performance Target
- **NMP speedup:** 1.25-1.7× faster than host baseline
- **Host latency:** ~382ns per access (through CXL Bridge + Device)
- **NMP latency:** ~230-305ns per access (direct to memory bus)

### Key Statistics to Check

After both experiments complete, compare:

```bash
# Host cycles
grep 'board.processor.switch.core.0.numCycles' m5out_host/stats.txt

# NMP cycles
grep 'board.processor.switch.core.1.numCycles' m5out_nmp/stats.txt

# Calculate speedup
# Speedup = Host_Cycles / NMP_Cycles
# Expected: 1.25 - 1.7
```

### Routing Verification

Check that the routing is working correctly:

```bash
# Host should use CXL Bridge
grep 'board.bridge' m5out_host/stats.txt | grep -E 'numReads|numWrites'

# NMP should use NMP Bridge (Core 1)
grep 'board.nmp_bridge' m5out_nmp/stats.txt | grep -E 'numReads|numWrites'

# NMP: CXL Bridge should be idle (Core 0 not used)
grep 'board.bridge' m5out_nmp/stats.txt | grep -E 'numReads|numWrites'
```

## Timeline ⏱️

- **Benchmark installation:** 2-5 minutes
- **NMP experiment:** 30 minutes - 2 hours (depending on system)
- **Host baseline:** 30 minutes - 2 hours (depending on system)
- **Total:** ~1-4 hours

## Troubleshooting 🔍

### If experiments fail with "benchmark not found"
- The benchmark wasn't installed to the disk image
- Run the setup script (Option 1 above)

### If both experiments show similar performance
1. Check that NMP routing is enabled (look for NMP debug output)
2. Verify address ranges in config.ini
3. Check that benchmark is actually accessing CXL memory (L3 misses)

### Monitor progress in real-time

```bash
# NMP experiment
tail -f m5out_nmp/simout.txt

# Host baseline
tail -f m5out_host/simout.txt
```

## Files Changed in This Session

1. **src/python/gem5/components/boards/x86_board.py**
   - Lines 249, 252: Fixed Addr formatting (int() conversion)

2. **benchmarks/memory_stride_access.c**
   - Changed to 128MB array size
   - Added mmap() with MAP_FIXED for specific addresses
   - Added NMP_CORE environment variable support
   - Address ranges: 0x100000000 (host), 0x300000000 (NMP)

3. **configs/example/gem5_library/x86-cxl-nmp-run.py**
   - Uses NMP_CORE=1 environment variable
   - Runs with taskset -c 1

4. **configs/example/gem5_library/x86-cxl-host-run.py**
   - Uses NMP_CORE=0 environment variable
   - Single core configuration

## Quick Command Reference

```bash
# Install benchmark
cd benchmarks && sudo ./setup_benchmark.sh

# Run both experiments
cd ..
./run_nmp_experiment.sh memory_stride_access TIMING ASIC
./run_host_baseline.sh memory_stride_access TIMING ASIC

# Compare results
grep 'numCycles' m5out_host/stats.txt | head -5
grep 'numCycles' m5out_nmp/stats.txt | head -5
```

---

**Status:** Ready to install benchmark and run experiments! ✨
