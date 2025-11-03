# NMP Memory Stride Benchmark

Simple memory benchmark for testing Near-Memory Processing (NMP) speedup in CXL-DMSim.

## Overview

This benchmark performs strided memory accesses to CXL memory with a pseudo-random access pattern that defeats hardware prefetchers, ensuring that each access goes to memory.

### Benchmark Characteristics

- **Target Memory**: CXL memory at `0x100000000` (from your configuration)
- **Working Set**: 2GB (larger than 96MB LLC to ensure cache misses)
- **Access Pattern**: Pseudo-random stride (prime number multiplier)
- **Stride Size**: 64 bytes (cache line size)
- **Total Accesses**: 10,000 memory reads
- **gem5 Integration**: Uses m5ops for accurate statistics collection

## Quick Start

### 1. Build and Install

```bash
cd benchmarks
./setup_benchmark.sh
```

This script will:
- Build the m5ops library (if needed)
- Compile the benchmark with gem5 support
- Mount the disk image
- Install the benchmark to `/home/cxl_benchmark/memory_stride_access`
- Unmount the disk image

### 2. Run NMP Experiment

```bash
cd ..
./run_nmp_experiments.sh memory_stride_access TIMING ASIC
```

### 3. View Results

```bash
less output/nmp_memory_stride_access_TIMING_ASIC/simout.txt
```

Look for:
```
=== Running Host CPU (Core 0) Baseline ===
Average latency: XXX ns per access

=== Running NMP CPU (Core 1) Test ===
Average latency: YYY ns per access
```

Expected speedup: Core 1 should be 1.25-1.7× faster than Core 0.

## Manual Build Instructions

If you prefer to build manually:

```bash
cd benchmarks

# Build without m5ops (for testing outside gem5)
make memory_stride_access

# Build with m5ops (for gem5)
make memory_stride_access_m5

# Install to disk image (requires mounted disk)
sudo mount -o loop,offset=$((2048*512)) ../fs_files/parsec.img /tmp/parsec_mount
make install
sudo umount /tmp/parsec_mount
```

## Files

- `memory_stride_access.c` - Benchmark source code
- `Makefile` - Build configuration
- `setup_benchmark.sh` - Automated setup script
- `README.md` - This file

## Benchmark Details

### Memory Access Pattern

The benchmark uses a pseudo-random stride pattern:

```c
for (int i = 0; i < 10000; i++) {
    size_t idx = ((i * 7919) % (SIZE / STRIDE)) * STRIDE;
    sum += data[idx];  // Read from CXL memory
}
```

- Prime number (7919) creates irregular access pattern
- Defeats hardware prefetchers
- Forces actual memory accesses
- Each access is cache line aligned (64 bytes)

### gem5 Integration

The benchmark uses m5ops for accurate measurement:

- `m5_reset_stats()` - Reset statistics before benchmark
- `m5_work_begin()` - Mark beginning of region of interest
- `m5_work_end()` - Mark end of region of interest
- `m5_dump_stats()` - Dump statistics after benchmark

### Expected Results

| Configuration | Path | Expected Latency |
|---------------|------|------------------|
| Core 0 (Host) | L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus | ~382 ns |
| Core 1 (NMP)  | L3 → MemBus → NMPBridge → CXL_Mem_Bus (DIRECT) | ~230-305 ns |

**Speedup**: 1.25-1.7× (bypassing ~77ns CXL protocol overhead)

## Troubleshooting

### Issue: Benchmark not found during simulation

**Error**: `sh: /home/cxl_benchmark/memory_stride_access: not found`

**Solution**:
```bash
# Verify installation
sudo mount -o loop,offset=$((2048*512)) ../fs_files/parsec.img /tmp/parsec_mount
ls -lh /tmp/parsec_mount/home/cxl_benchmark/memory_stride_access
sudo umount /tmp/parsec_mount
```

### Issue: Segmentation fault

**Error**: `Segmentation fault (core dumped)`

**Cause**: Direct memory access to `0x100000000` requires proper memory mapping in gem5.

**Solution**: Ensure gem5 configuration properly maps CXL memory at this address. Check `config.ini`:
```ini
[board.cxl_dram]
range=0x100000000:0x2FFFFFFFF
```

### Issue: m5ops library not found

**Error**: `fatal error: gem5/m5ops.h: No such file or directory`

**Solution**: Build m5ops library:
```bash
cd ../util/m5
scons build/x86/out/libm5.a
```

## Performance Analysis

After running the benchmark, analyze the gem5 statistics:

```bash
# Core 0 (Host) statistics
grep "board.processor.switch.core.0" output/nmp_*/stats.txt | grep -E "numCycles|committedInsts"

# Core 1 (NMP) statistics
grep "board.processor.switch.core.1" output/nmp_*/stats.txt | grep -E "numCycles|committedInsts"

# CXL Bridge activity (should show Core 0 only)
grep "board.bridge" output/nmp_*/stats.txt | grep -E "numReads|numWrites"

# NMP Bridge activity (should show Core 1 only)
grep "board.nmp_bridge" output/nmp_*/stats.txt | grep -E "numReads|numWrites"
```

Calculate average memory latency:
```
latency = (total_cycles * clock_period) / memory_accesses
```

With 2.4 GHz clock (416 ps period):
```
latency_ns = (cycles / accesses) * 0.416
```

## Contributing

To modify the benchmark:

1. Edit `memory_stride_access.c`
2. Rebuild: `make clean && make memory_stride_access_m5`
3. Reinstall: `./setup_benchmark.sh`
4. Re-run experiment: `../run_nmp_experiments.sh memory_stride_access TIMING ASIC`

## References

- gem5 documentation: https://www.gem5.org/documentation/
- m5ops guide: https://www.gem5.org/documentation/general_docs/m5ops/
- CXL-DMSim paper: `../CXL-DMSim_A_Full-System_CXL_Disaggregated_Memory_Simulator.pdf`
- Research context: `../RESEARCH_CONTEXT.md`
- NMP implementation: `../NMP_IMPLEMENTATION_SUMMARY.md`
