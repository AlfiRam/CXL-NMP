# CPU Verification Commands - Quick Reference

This document shows how to verify CPU configuration in the simulated OS.

## Commands Added to Run Configurations

Both NMP and Host configurations now include these CPU verification commands:

```bash
nproc                                    # Shows number of processing units
cat /proc/cpuinfo | grep '^processor'    # Shows all processors
cat /sys/devices/system/cpu/online       # Shows online CPU range
```

## Expected Output

### NMP Experiment (2 Cores):
```
=== NMP Test (Core 1, Direct CXL Path) ===
Cores: 2 CPUs (Core 0 idle, Core 1 active)
Path: L3 → MemBus → NMPBridge → CXL_Mem_Bus (DIRECT)
Expected latency: ~230-305ns per access (1.25-1.7x faster than host)

[CPU INFO] Checking available CPUs in OS:
Number of CPUs: 2
CPU List:
  CPU 0
  CPU 1
Online CPUs: 0-1

[TASKSET] Pinning workload to Core 1 (NMP path)
[TASKSET] Command: NMP_CORE=1 taskset -c 1 /home/cxl_benchmark/memory_stride_access

=== Memory Stride Benchmark ===
Core type: NMP (Core 1)
Base address: 0x300000000
Size: 128 MB
*** RUNNING ON CPU: 1 ***
CPU Affinity Mask: CPU1
CONFIRMED: Process is pinned to single CPU
================================
```

### Host Experiment (1 Core):
```
=== Host Baseline (Single Core, Normal CXL Path) ===
Core: 1 CPU (Core 0)
Path: L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus
Expected latency: ~382ns per access

[CPU INFO] Checking available CPUs in OS:
Number of CPUs: 1
CPU List:
  CPU 0
Online CPUs: 0

[WORKLOAD] Running on Core 0 (Host path, no taskset needed)
[WORKLOAD] Command: NMP_CORE=0 /home/cxl_benchmark/memory_stride_access

=== Memory Stride Benchmark ===
Core type: Host (Core 0)
Base address: 0x100000000
Size: 128 MB
*** RUNNING ON CPU: 0 ***
CPU Affinity Mask: CPU0
CONFIRMED: Process is pinned to single CPU
================================
```

## What Each Command Shows

### `nproc`
- **Purpose:** Count total number of processing units available
- **NMP Expected:** `2`
- **Host Expected:** `1`

### `cat /proc/cpuinfo | grep '^processor'`
- **Purpose:** List all processor IDs from kernel's perspective
- **NMP Expected:** Shows `processor : 0` and `processor : 1`
- **Host Expected:** Shows only `processor : 0`
- **Source:** Linux kernel's CPU database

### `cat /sys/devices/system/cpu/online`
- **Purpose:** Show range of online CPUs
- **NMP Expected:** `0-1` (CPUs 0 and 1 are online)
- **Host Expected:** `0` (only CPU 0 is online)
- **Source:** Linux sysfs (kernel's device model)

## Additional Manual Verification Commands

If you want to verify manually inside the simulation terminal, you can run:

```bash
# Show detailed CPU info
lscpu

# Show all CPU info from /proc
cat /proc/cpuinfo

# Check specific CPU directories exist
ls -la /sys/devices/system/cpu/cpu*

# Test taskset on each CPU
taskset -c 0 echo "Running on CPU 0"
taskset -c 1 echo "Running on CPU 1"  # Only works in NMP (2-core)

# Show current process CPU affinity
taskset -p $$
```

## Troubleshooting

### Issue: NMP shows only 1 CPU
**Symptom:** `nproc` returns 1, only CPU 0 listed
**Problem:** Board not configured with 2 cores
**Check:** Look for `num_cores=2` in config script

### Issue: CPU affinity shows all CPUs
**Symptom:** `CPU Affinity Mask: CPU0 CPU1`
**Problem:** taskset didn't pin to single CPU
**Check:** Verify `taskset -c 1` in command

### Issue: Benchmark runs on wrong CPU
**Symptom:** `*** RUNNING ON CPU: 0 ***` in NMP experiment
**Problem:** taskset command not working or not present
**Check:** Look for `[TASKSET]` confirmation message

## Quick Verification Checklist

After boot, before benchmark runs, verify:

### NMP (2 cores):
- [ ] `nproc` shows `2`
- [ ] CPU List shows `CPU 0` and `CPU 1`
- [ ] Online CPUs shows `0-1`
- [ ] Benchmark shows `*** RUNNING ON CPU: 1 ***`
- [ ] CPU Affinity Mask shows only `CPU1`

### Host (1 core):
- [ ] `nproc` shows `1`
- [ ] CPU List shows only `CPU 0`
- [ ] Online CPUs shows `0`
- [ ] Benchmark shows `*** RUNNING ON CPU: 0 ***`
- [ ] CPU Affinity Mask shows only `CPU0`

---

**These checks provide OS-level verification that complements gem5's statistics!** ✅
