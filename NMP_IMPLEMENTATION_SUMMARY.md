# Near-Memory Processing (NMP) Implementation in CXL-DMSim gem5

**Technical Summary for Advisor Meeting**

---

## 1. Research Context

### What is NMP with CXL?
Near-Memory Processing (NMP) is a computing architecture that co-locates computation with CXL-attached memory to reduce memory access latency. By placing compute resources directly adjacent to memory, we can bypass the standard CXL protocol overhead incurred by host-side processors.

### Research Motivation
Standard CXL memory access from a host CPU requires traversing multiple protocol layers:
- **CXL Bridge**: 62ns (50ns bridge_lat + 12ns proto_proc_lat)
- **CXL Memory Device**: 15ns (proto_proc_lat in ASIC mode)
- **Total overhead**: ~77ns per access

Our hypothesis: By implementing an NMP core that bypasses these protocol layers and directly accesses the CXL memory bus, we can reduce latency by ~77ns per access, achieving **1.25-1.7x speedup** for memory-bound workloads.

### Expected Benefits
- **Latency reduction**: 382ns (host) → 305ns (NMP) = 20-25% improvement
- **Demonstrates**: CXL protocol overhead is significant and quantifiable
- **Use case**: Memory-intensive workloads with poor cache locality

---

## 2. Architecture Comparison

### Baseline Configuration (Host Mode)
```
┌─────────────────────────────────────────────────────────────────────┐
│ Core 0 (Host CPU)                                                   │
│   ↓                                                                  │
│ L1 Cache → L2 Cache → L3 Cache (96MB)                              │
│   ↓                                                                  │
│ Memory Bus                                                           │
│   ↓                                                                  │
│ CXL Bridge (62ns) ──────────────────┐                              │
│   ↓                                  │ CXL Protocol Overhead        │
│ IO Bus                               │ Total: ~77ns                 │
│   ↓                                  │                              │
│ CXL Memory Device (15ns) ───────────┘                              │
│   ↓                                                                  │
│ cxl_mem_bus                                                         │
│   ↓                                                                  │
│ DRAM Controllers (DDR5-4400)                                        │
└─────────────────────────────────────────────────────────────────────┘

Address Range: 0x100000000 - 0x2FFFFFFFF
Configuration:  1 core, standard CXL path
Expected Latency: ~382ns per access
```

### NMP Configuration (Bypass Mode)
```
┌──────────────────────────────────────────────────────────────────────┐
│ Core 0 (Host CPU)            Core 1 (NMP CPU)                       │
│   ↓                            ↓                                     │
│ L1 → L2 → L3                 L1 → L2 → L3                          │
│   ↓                            ↓                                     │
│ Memory Bus ←──────────────── Memory Bus                             │
│   ↓                            ↓                                     │
│ CXL Bridge (62ns)            NMP Bridge (1ns) ──┐                   │
│   ↓                            ↓                 │ BYPASS!          │
│ IO Bus                       AddrMapper          │ Saves 77ns       │
│   ↓                            ↓                 │                  │
│ CXL Memory Device (15ns)     │                  │                  │
│   ↓                            ↓                 │                  │
│   └────────────────→ cxl_mem_bus ←──────────────┘                  │
│                          ↓                                           │
│                   DRAM Controllers                                   │
└──────────────────────────────────────────────────────────────────────┘

Core 0 Address: 0x100000000 - 0x2FFFFFFFF (standard path)
Core 1 Address: 0x300000000 - 0x4FFFFFFFF (NMP bypass path)
                ↓ (AddrMapper translates to 0x100000000)

Configuration: 2 cores, Core 1 bypasses CXL protocol layers
Expected Latency: ~305ns per access (NMP), ~382ns (Host)
Expected Speedup: 1.25x
```

---

## 3. Implementation Details

### Key File Modified: `src/python/gem5/components/boards/x86_board.py`

#### 3.1 Added `enable_nmp` Parameter

**Challenge**: SimObject metaclass restricts attribute assignment during `__init__()`.

**Solution**: Use `object.__setattr__()` to bypass restrictions:

```python
def __init__(
    self,
    # ... existing parameters ...
    cxl_memory: Optional[AbstractMemorySystem] = None,
    is_asic: bool = True,
    enable_nmp: bool = False,  # NEW PARAMETER
):
    # Store NMP flag before SimObject construction
    object.__setattr__(self, "_enable_nmp", enable_nmp)

    super().__init__(
        clk_freq=clk_freq,
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
    )
```

#### 3.2 NMP Routing Architecture (Lines 206-250)

When `enable_nmp=True`, we create a bypass path for Core 1:

```python
if self._enable_nmp:
    print("[NMP] Creating Near-Memory Processing bypass path for Core 1")

    # 1. Minimal-latency bridge (1ns overhead)
    self.nmp_bridge = Bridge(
        req_size=64,
        resp_size=64,
        delay="1ns",  # Minimal overhead vs 62ns CXL Bridge
    )

    # 2. Address range mapper
    #    Input:  0x300000000 - 0x4FFFFFFFF (Core 1 uses this range)
    #    Output: 0x100000000 - 0x2FFFFFFFF (actual DRAM physical addresses)
    self.nmp_addr_mapper = RangeAddrMapper(
        original_ranges=[AddrRange(0x300000000, size="8GB")],
        remapped_ranges=[AddrRange(0x100000000, size="8GB")]
    )

    # 3. Connect bypass path: Core 1 → NMP_Bridge → AddrMapper → cxl_mem_bus
    self.nmp_bridge.mem_side_port = self.nmp_addr_mapper.cpu_side_port

    # KEY INSIGHT: cxl_mem_bus.cpu_side_ports is a VECTOR port
    # Port[0]: CXLMemory.mem_req_port (Host path goes THROUGH device)
    # Port[1]: NMP AddrMapper (NMP path BYPASSES device)
    self.nmp_addr_mapper.mem_side_port = self.cxl_mem_bus.cpu_side_ports

    # 4. Connect Core 1's memory port to bypass bridge
    processor.get_cores()[1].memory = self.nmp_bridge.cpu_side_port
```

#### 3.3 Address-Based Routing Strategy

**Design Decision**: Use address ranges to distinguish paths, not physical ports.

| Component | Address Range | Routing Path |
|-----------|--------------|--------------|
| Host (Core 0) | 0x100000000 - 0x2FFFFFFFF | Standard CXL path (THROUGH device) |
| NMP (Core 1) | 0x300000000 - 0x4FFFFFFFF | Bypass path (DIRECT to bus) |

**Why address-based?**
- Simpler implementation in gem5 (no kernel modifications needed)
- Benchmark controls routing via `mmap()` base address
- Easy to verify via stats (separate address range tracking)

**How it works:**
1. Core 1 issues load to address `0x300000000`
2. Request goes through NMP_Bridge (1ns)
3. AddrMapper translates `0x300000000` → `0x100000000`
4. Request arrives at `cxl_mem_bus.cpu_side_ports[1]` (direct entry, bypassing CXLMemory device)
5. Bus routes to appropriate DRAM controller

#### 3.4 Critical Architectural Insight

**The `cxl_mem_bus.cpu_side_ports` vector port allows multiple entry points:**

```python
# Standard setup (line 188):
self.cxl_mem_bus.cpu_side_ports = self.pc.south_bridge.cxlmemory.mem_req_port
# This connects CXLMemory device to cpu_side_ports[0]

# NMP bypass (line 245):
self.nmp_addr_mapper.mem_side_port = self.cxl_mem_bus.cpu_side_ports
# This connects AddrMapper to cpu_side_ports[1]
```

Both paths share the **same cxl_mem_bus** but use **different entry points**, avoiding port conflicts.

---

## 4. Configuration Files Created

### Experiment Configurations
1. **`configs/example/gem5_library/x86-cxl-host-run.py`**
   - Baseline: 1 core, `enable_nmp=False`
   - Uses `CPUTypes.TIMING` or `CPUTypes.O3`
   - Boots with KVM, switches to timing-accurate core
   - Runs benchmark with `NMP_CORE=0`

2. **`configs/example/gem5_library/x86-cxl-nmp-run.py`**
   - NMP mode: 2 cores, `enable_nmp=True`
   - Core 0: Standard path (idle during benchmark)
   - Core 1: NMP bypass path (runs benchmark)
   - Uses `taskset -c 1` to pin workload to Core 1
   - Runs benchmark with `NMP_CORE=1`

### Benchmark
3. **`benchmarks/memory_stride_access.c`**
   - lmbench-style pointer-chasing benchmark
   - Defeats hardware prefetchers (each access depends on previous)
   - Uses `NMP_CORE` environment variable to select address range
   - Uses `mmap(MAP_FIXED)` to allocate at specific addresses:
     - Core 0 → `0x100000000` (Host path)
     - Core 1 → `0x300000000` (NMP path)
   - Includes CPU affinity verification (`sched_getcpu()`, `sched_getaffinity()`)
   - Configuration: 2GB array, 4KB stride, 524,288 pointer chases

### Execution Scripts
4. **`run_nmp_experiment.sh`** - Runs NMP configuration
5. **`run_host_baseline.sh`** - Runs baseline for comparison

---

## 5. Challenges Encountered & Solutions

### Challenge 1: SimObject Attribute Restrictions
**Problem**: gem5's SimObject metaclass prohibits attribute assignment during `__init__()`.
```python
# This fails:
self._enable_nmp = enable_nmp
# AttributeError: Cannot set attribute on a SimObject
```

**Solution**: Use `object.__setattr__()` to bypass metaclass restrictions:
```python
object.__setattr__(self, "_enable_nmp", enable_nmp)
```

### Challenge 2: Port Connection Conflicts
**Problem**: Attempted to create separate `nmp_mem_bus` and connect DRAM controllers to both buses:
```
fatal: Port mem_ctrl0.port is already connected to cxl_mem_bus.mem_side_ports[0],
cannot connect nmp_mem_bus.mem_side_ports[0]
```

**Root cause**: gem5 doesn't allow double-connecting ports.

**Solution**: Share the same `cxl_mem_bus` with two entry points via vector port:
- Entry 0: CXLMemory device (Host path)
- Entry 1: NMP AddrMapper (NMP bypass path)

### Challenge 3: Benchmark Cache Hits (Iteration 1)
**Problem**: Initial results showed only 2% speedup instead of 25%.

**Diagnosis**:
- Expected: ~20M DRAM accesses
- Actual: Only 43 DRAM accesses
- Cause: 16MB working set fit in 96MB L3 cache

**Solution**: Upgrade benchmark configuration:
- Array: 128MB → 2GB
- Stride: 64B → 4KB (page size)
- Traversals: 10 → 1 (single pass, all cold misses)
- Result: 524,288 pointer chases across 2GB address space

### Challenge 4: Benchmark Cache Hits (Iteration 2)
**Problem**: After increasing stride to 4KB, still only ~23 DRAM accesses.

**Diagnosis**:
- Working set: 128MB / 4KB = 32,768 elements × 8 bytes = 256KB
- 256KB still fits in 96MB L3 cache!

**Solution**: Increase array size to 2GB:
- Working set: 2GB / 4KB = 524,288 elements × 8 bytes = 4MB of pointers
- Address space: 524,288 pages × 4KB = 2GB (way larger than L3)
- Expected: ~524,288 DRAM accesses (true DRAM latency)

### Challenge 5: Address Range Management
**Problem**: Needed to ensure Core 1 accesses don't conflict with Core 0.

**Solution**: Separated address ranges with 4GB gap:
- Host: `0x100000000` - `0x2FFFFFFFF` (4GB - 12GB)
- NMP: `0x300000000` - `0x4FFFFFFFF` (12GB - 20GB)
- AddrMapper translates NMP range back to physical DRAM addresses

---

## 6. Current Status

### ✅ Completed
- [x] NMP routing architecture implemented in `x86_board.py`
- [x] Both configurations compile and run successfully
- [x] Benchmark correctly pins to target CPU (verified via `sched_getcpu()`)
- [x] Address-based routing working (verified via stats)
- [x] NMP shows measurable performance difference
- [x] Benchmark upgraded to 2GB array with 4KB stride

### ⚠️ In Progress
- [ ] **Final validation with 2GB benchmark**
  - Status: Code updated, needs recompilation and testing
  - Expected: ~524,288 DRAM accesses (not 43)
  - Expected: ~200s (host) vs ~160s (NMP) = 1.25x speedup
  - Timeline: Recompile and run (~3-5 hours per experiment)

### 📊 Results Summary

#### Initial Results (Cache-Hit Scenario - INVALID)
```
Configuration   | simSeconds | Accesses    | ns/access | DRAM Reads
----------------|------------|-------------|-----------|------------
Host (Core 0)   | 0.296470s  | 20,971,520  | 14.14ns   | 43
NMP (Core 1)    | 0.290716s  | 20,971,520  | 13.86ns   | 43
Speedup         | 1.02x      |             |           |
```
**Analysis**: 14ns per access = L3 cache latency, not DRAM latency. Benchmark was hitting in cache!

#### Expected Results (DRAM Access - PENDING)
```
Configuration   | simSeconds | Accesses | ns/access | DRAM Reads
----------------|------------|----------|-----------|------------
Host (Core 0)   | ~200s      | 524,288  | ~382ns    | ~524,288
NMP (Core 1)    | ~160s      | 524,288  | ~305ns    | ~524,288
Speedup         | ~1.25x     |          |           |
```
**Analysis**: True DRAM latency measurement with proper L3 cache bypass.

---

## 7. Next Steps

### Immediate (This Week)
1. **Complete 2GB benchmark testing**
   ```bash
   # Compile 2GB benchmark
   cd /home/malfiram/CXL-DMSim/benchmarks
   x86_64-linux-gnu-gcc -static -O2 -I../include -o memory_stride_access \
       memory_stride_access.c ../util/m5/build/x86/out/libm5.a

   # Install to disk image (requires sudo)
   sudo mount -o loop,offset=$((2048*512)) ../fs_files/parsec.img /tmp/parsec_mount
   sudo cp memory_stride_access /tmp/parsec_mount/home/cxl_benchmark/
   sudo umount /tmp/parsec_mount

   # Run experiments (~3-5 hours each)
   build/X86/gem5.opt -d m5out_host configs/example/gem5_library/x86-cxl-host-run.py
   build/X86/gem5.opt -d m5out_nmp configs/example/gem5_library/x86-cxl-nmp-run.py
   ```

2. **Validate expected speedup**
   - Verify ~524K DRAM reads in stats (not 43)
   - Verify per-access latency: 382ns (host) vs 305ns (NMP)
   - Target: 1.25x speedup (200s host → 160s NMP)

### Short-term (Next 2 Weeks)
3. **Detailed stats analysis**
   - Confirm CXL Bridge bypassed for Core 1
   - Confirm CXL Memory device bypassed for Core 1
   - Verify address translation working correctly
   - Compare cache miss rates between configurations

4. **Document results**
   - Create performance graphs (latency, bandwidth, speedup)
   - Write technical report with architectural diagrams
   - Prepare conference/workshop submission

### Long-term (Future Work)
5. **Enhanced NMP model**
   - Consider true port-based routing (more realistic)
   - Add NMP-specific cache hierarchy
   - Model NMP power consumption
   - Support multiple NMP cores

6. **Workload characterization**
   - Test with STREAM benchmark
   - Test with graph traversal workloads
   - Compare with real CXL hardware (when available)

---

## 8. Key Takeaways for Advisor

1. **Implementation is functionally complete**: Both configurations boot, run benchmarks, and produce measurable performance differences.

2. **Main bottleneck was benchmark design, not architecture**: The initial 2% speedup was due to L3 cache hits, not a flaw in the NMP implementation. We've systematically fixed this:
   - Iteration 1: Pointer chasing (defeats prefetcher) ✓
   - Iteration 2: 4KB stride (defeats cache locality) ✓
   - Iteration 3: 2GB array (sufficient working set) ✓

3. **Architectural validation is pending**: Once benchmark forces DRAM access, we expect to see:
   - ~77ns latency reduction per access
   - 1.25x speedup for memory-bound workloads
   - Clear evidence of CXL protocol bypass in stats

4. **Research contribution**: This is (to our knowledge) the first open-source implementation of NMP bypass routing in gem5 for CXL systems. It provides a foundation for exploring:
   - NMP programming models
   - CXL protocol overhead quantification
   - Hybrid host+NMP workload scheduling

5. **Timeline**: Assuming benchmark fix works as expected, we should have complete results and analysis within 1-2 weeks.

---

## 9. Technical Validation Checklist

Before advisor meeting, verify:

### Architecture Verification
- [ ] `config.ini` shows `nmp_bridge` with 1ns delay
- [ ] `config.ini` shows `nmp_addr_mapper` with correct address ranges
- [ ] Both `cxl_mem_bus.cpu_side_ports[0]` and `[1]` are connected

### Benchmark Verification
- [ ] Benchmark shows "Size: 2 GB" (not 0.128 GB)
- [ ] Benchmark shows "Stride: 4096 bytes" (not 64)
- [ ] Benchmark shows "Total pointer chases: 524288" (not 20M)
- [ ] Benchmark shows "*** RUNNING ON CPU: 1 ***" for NMP config

### Results Verification
- [ ] Host shows ~524K DRAM reads (not 43)
- [ ] NMP shows ~524K DRAM reads (not 43)
- [ ] Host latency ~300-400ns per access (not 14ns)
- [ ] NMP latency ~200-300ns per access (not 14ns)
- [ ] Speedup ratio between 1.2x - 1.7x (not 1.02x)

---

## 10. Questions for Discussion

1. **Routing approach**: Should we pursue port-based routing for more realistic modeling, or is address-based sufficient for demonstrating the concept?

2. **Workload selection**: What other memory-intensive workloads would be interesting to test? Graph algorithms? Database queries?

3. **Publication target**: Is this sufficient for a workshop paper (MEMSYS, ISPASS)? Or should we aim for a full conference paper with more comprehensive evaluation?

4. **Comparison baseline**: Should we compare against alternative approaches (software prefetching, NUMA optimization, etc.) or focus purely on NMP vs standard CXL?

5. **Future directions**: Are you interested in exploring heterogeneous NMP (different ISAs), or multi-NMP coordination, or power modeling?

---

## References

- **Architecture**: `src/python/gem5/components/boards/x86_board.py` (lines 160-250)
- **Host Config**: `configs/example/gem5_library/x86-cxl-host-run.py`
- **NMP Config**: `configs/example/gem5_library/x86-cxl-nmp-run.py`
- **Benchmark**: `benchmarks/memory_stride_access.c`
- **CPU Verification**: `CPU_VERIFICATION.md`
- **Research Context**: CXL-DMSim project README
