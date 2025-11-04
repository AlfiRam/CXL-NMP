# NMP Core Assignment Verification Guide

This guide shows how to verify with **absolute certainty** that the workload ran on the correct CPU core.

## 1. Runtime Verification (In Simulated Terminal)

When the benchmark runs, you'll see this output:

### NMP Experiment (Core 1):
```
[TASKSET] Pinning workload to Core 1 (NMP path)
[TASKSET] Command: NMP_CORE=1 taskset -c 1 /home/cxl_benchmark/memory_stride_access

=== Memory Stride Benchmark ===
Core type: NMP (Core 1)
Base address: 0x300000000
Size: 128 MB
*** RUNNING ON CPU: 1 ***                    ← sched_getcpu() proof
CPU Affinity Mask: CPU1                      ← Only CPU 1 is allowed
CONFIRMED: Process is pinned to single CPU   ← taskset worked!
================================
Memory mapped at: 0x300000000
Initializing memory...
Benchmark complete
Checksum: 0
Final CPU check: 1 (STABLE - stayed on CPU 1) ← Still on CPU 1 at end
```

### Host Experiment (Core 0):
```
[WORKLOAD] Running on Core 0 (Host path, no taskset needed)
[WORKLOAD] Command: NMP_CORE=0 /home/cxl_benchmark/memory_stride_access

=== Memory Stride Benchmark ===
Core type: Host (Core 0)
Base address: 0x100000000
Size: 128 MB
*** RUNNING ON CPU: 0 ***                    ← sched_getcpu() proof
CPU Affinity Mask: CPU0                      ← Only CPU 0 (single core system)
CONFIRMED: Process is pinned to single CPU
================================
Memory mapped at: 0x100000000
Initializing memory...
Benchmark complete
Checksum: 0
Final CPU check: 0 (STABLE - stayed on CPU 0) ← Still on CPU 0 at end
```

## 2. What Each Check Means

### `sched_getcpu()` - Linux Kernel Query
- **What it does:** Directly asks the Linux kernel "which CPU am I running on right now?"
- **Why it's definitive:** This is a system call that returns the actual physical CPU number
- **Location:** Called at benchmark start and end

### `sched_getaffinity()` - CPU Affinity Mask
- **What it does:** Shows which CPUs the process is ALLOWED to run on
- **Why it's definitive:** If only one CPU is in the mask, the process is pinned
- **Example outputs:**
  - `CPU Affinity Mask: CPU1` = Only CPU 1 (NMP experiment) ✅
  - `CPU Affinity Mask: CPU0 CPU1` = Can run on both (taskset failed) ❌

### Final CPU Check
- **What it does:** Calls `sched_getcpu()` again after benchmark completes
- **Why it matters:** Confirms the process didn't migrate to another CPU during execution
- **Expected:** `STABLE - stayed on CPU X`

## 3. gem5 Statistics Verification

After the simulation completes, verify using gem5 stats:

### Check Core Activity

```bash
# NMP Experiment - Core 1 should be active, Core 0 idle
grep 'board.processor.switch.core.*.numCycles' m5out_nmp/stats.txt

Expected output:
board.processor.switch.core.0.numCycles          12345    ← Low (idle)
board.processor.switch.core.1.numCycles          9876543  ← High (active)
```

### Check Bridge Usage

```bash
# NMP Experiment - NMP bridge should be used, CXL bridge idle
grep 'board.*bridge.*Reads' m5out_nmp/stats.txt

Expected output:
board.bridge.numReads                           0         ← Host bridge unused
board.nmp_bridge.numReads                       15625     ← NMP bridge used
```

### Check L3 Cache Misses

```bash
# Verify Core 1 has cache misses (doing the work)
grep 'board.processor.switch.core.*.dcache.overallMisses::total' m5out_nmp/stats.txt

Expected output:
board.processor.switch.core.0.dcache.overallMisses::total     0      ← Core 0 idle
board.processor.switch.core.1.dcache.overallMisses::total     15625  ← Core 1 active
```

## 4. Complete Verification Checklist

After running an experiment, verify ALL of these:

### ✅ NMP Experiment (Core 1)
- [ ] Terminal shows: `*** RUNNING ON CPU: 1 ***`
- [ ] Terminal shows: `CPU Affinity Mask: CPU1` (only one CPU)
- [ ] Terminal shows: `CONFIRMED: Process is pinned to single CPU`
- [ ] Terminal shows: `Final CPU check: 1 (STABLE - stayed on CPU 1)`
- [ ] Stats show: `core.1.numCycles >> core.0.numCycles`
- [ ] Stats show: `board.nmp_bridge.numReads > 0`
- [ ] Stats show: `board.bridge.numReads == 0` (or very small)

### ✅ Host Experiment (Core 0)
- [ ] Terminal shows: `*** RUNNING ON CPU: 0 ***`
- [ ] Terminal shows: `CPU Affinity Mask: CPU0`
- [ ] Terminal shows: `CONFIRMED: Process is pinned to single CPU`
- [ ] Terminal shows: `Final CPU check: 0 (STABLE - stayed on CPU 0)`
- [ ] Stats show: `core.0.numCycles` is high (only one core exists)
- [ ] Stats show: `board.bridge.numReads > 0` (uses normal CXL bridge)

## 5. Common Issues & Solutions

### Issue: `CPU Affinity Mask: CPU0 CPU1` (both CPUs)
**Problem:** taskset didn't work
**Solution:** Check the command includes `taskset -c 1`

### Issue: `MIGRATED - started on CPU 0!`
**Problem:** Process moved to different CPU during execution
**Solution:** This shouldn't happen in gem5 simulation, but indicates taskset issue

### Issue: Both cores show similar numCycles
**Problem:** Workload isn't running on the correct core
**Solution:** Verify NMP_CORE environment variable is set correctly

## 6. Quick Verification Script

```bash
#!/bin/bash
# Save as verify_nmp.sh

echo "=== NMP Experiment Verification ==="
echo ""
echo "1. Runtime CPU Check:"
grep "RUNNING ON CPU" m5out_nmp/simout.txt

echo ""
echo "2. CPU Affinity:"
grep "CPU Affinity Mask" m5out_nmp/simout.txt

echo ""
echo "3. Core Activity (cycles):"
grep 'board.processor.switch.core.*.numCycles' m5out_nmp/stats.txt

echo ""
echo "4. Bridge Usage:"
grep 'board.*bridge.*Reads' m5out_nmp/stats.txt | grep -v "system.iobus"

echo ""
echo "=== Host Experiment Verification ==="
echo ""
echo "1. Runtime CPU Check:"
grep "RUNNING ON CPU" m5out_host/simout.txt

echo ""
echo "2. Core Activity (cycles):"
grep 'board.processor.switch.core.*.numCycles' m5out_host/stats.txt

echo ""
echo "3. Bridge Usage:"
grep 'board.bridge.*Reads' m5out_host/stats.txt | grep -v "system.iobus"
```

Run with:
```bash
chmod +x verify_nmp.sh
./verify_nmp.sh
```

---

**Summary:** The combination of `sched_getcpu()`, `sched_getaffinity()`, and gem5 statistics provides **triple verification** that the workload ran on the correct core. If all checks pass, you have absolute certainty! ✅
