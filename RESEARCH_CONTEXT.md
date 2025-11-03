# Revised NMP Research Context for CXL-DMSim

## Research Background

### CXL-Based Disaggregated Memory
CXL (Compute eXpress Link) enables **memory disaggregation**: remote memory nodes form a pool that can be dynamically partitioned among processor hosts, maximizing memory utilization.

**CXL Protocols:**
- **CXL.io**: Device configuration, MMIO, interrupts (storage use)
- **CXL.cache**: Accelerators participate in cache coherence
- **CXL.mem**: Direct memory access by mapping remote memory into system address space
  - **Key for memory disaggregation** ← Our focus

### Why Near-Memory Processing (NMP) with CXL is Unique

**Three key advantages over local NMP:**

1. **CXL devices already have processing capability**
   - Must parse memory request packets from hosts
   - Already includes logic capable of general computation

2. **No power constraints like CPU socket**
   - Local memories share CPU socket power budget
   - CXL devices have separate power envelope
   - Can add processors without affecting host

3. **Higher latency makes NMP more beneficial** ⭐
   - Local DRAM: ~100-150 ns
   - CXL memory: 3-10× slower (~300-1500 ns)
   - **More latency = greater benefit from processing near data**
   - Moving data is expensive, moving computation is cheap

### Long-Term Vision: Secure Enclave Offloading

**Goal:** Offload data-intensive enclave execution to CXL memory using CXL.mem protocol

**Key challenges:**
- **Subtree migration**: Transfer integrity tree ownership from host to CXL device
  - Host self-invalidates integrity subtree
  - Securely send root to memory device
  - NMP processor works on data using local subtree

- **Completion signaling**: Host polls to know when offloaded task completes

**Unique opportunities:**
1. **Low-cost polling**: CXL controller withholds response until task complete
   - Host enters low power mode (no busy-waiting)
   - Avoids flooding memory hierarchy with poll requests

2. **Automatic counter tracking**: CXL controller sees all host writebacks
   - Can accurately track encryption counter values
   - **Avoid migrating coherence counters** (optimization!)

**Expected benefit:** Substantial performance gains for data-intensive secure workloads

---

## Current Phase: **BASELINE VALIDATION**

### Research Question
Does a processor co-located with CXL memory provide a latency advantage over remote execution from the host CPU?

### Hypothesis
NMP CPU (co-located with CXL memory) should access memory faster than Host CPU (remote access via CXL bridge and protocol)

### What We've Measured So Far
- Host CPU → CXL Memory: **382 ns per access** (measured via pointer-chasing benchmark)
- Configuration: NUMA Node 0 (Host CPU + 3GB DRAM), NUMA Node 1 (CXL device + 8GB memory)
- Array size: 2GB, Stride: 64 bytes, 10K random accesses

---

## Proposed NMP Architecture

### Implementation Strategy

Instead of creating separate configurations, we'll add an **NMP CPU core within the same system** that bypasses CXL protocol overhead by connecting directly to CXL memory.

### Architecture Modifications

#### 1. **Add Second CPU Core (NMP Core)**
```
Processor:
├─ Core 0 (Host CPU - existing)
│  ├─ L1 I-cache (32KB)
│  ├─ L1 D-cache (48KB)
│  └─ L2 cache (2MB)
│
└─ Core 1 (NMP CPU - NEW)
   ├─ L1 I-cache (32KB)
   ├─ L1 D-cache (48KB)
   └─ L2 cache (2MB)

Both cores share:
└─ L3 cache (96MB)
   └─ L3 Bus (CoherentXBar)
      └─ MemBus/SystemXBar (CoherentXBar - Point of Coherency)
```

#### 2. **Memory Access Path Differentiation**

**Host CPU (Core 0) Path - REMOTE ACCESS:**
```
Host CPU → L1 → L2 → L3 → L3 Bus → MemBus/SystemXBar
    → CXLBridge (50ns bridge + 12ns protocol = 62ns)
    → IOBus
    → CXLMemory device (15ns protocol processing)
    → CXLMemBar
    → CXL Memory Controllers (2 channels)
    → DDR5 Memory

Total CXL overhead: ~77ns + additional protocol/queuing
Measured total: 382ns per access
```

**NMP CPU (Core 1) Path - LOCAL ACCESS:**
```
NMP CPU → L1 → L2 → L3 → L3 Bus → MemBus/SystemXBar
    → **BYPASS CXLBridge (save 62ns)**
    → IOBus (or bypass entirely?)
    → **BYPASS CXLMemory device (save 15ns)**
    → Direct to CXLMemBar
    → CXL Memory Controllers
    → DDR5 Memory

Total bypass savings: ~77ns minimum
Expected latency: 382ns - 77ns ≈ 305ns (conservative)
Or ~230-280ns if additional overhead bypassed (optimistic)
```

#### 3. **Key Implementation Details**

**Port-Based Router Logic in MemBus/SystemXBar:**
- MemBus/SystemXBar is the "Point of Coherency" where routing decisions are made
- **Routing strategy:** Distinguish packets by their source port
- Core 0 (Host) packets arriving from L3 cache → route through CXLBridge (existing path)
- Core 1 (NMP) packets arriving from L3 cache → route directly to CXLMemBar, bypassing CXLBridge and CXLMemory device

**Implementation approach:**
```python
# Pseudocode for port-based routing in MemBus/SystemXBar
def route_packet(packet, source_port):
    # Check if packet originates from NMP core (Core 1)
    if source_port == board.cache_hierarchy.l3cache.mem_side[1]:  # Core 1's L3 path
        # NMP path: Direct to CXL memory bus, bypass bridge & device
        return board.cxl_mem_bus
    else:  # Core 0 (host)
        # Standard path: Through CXL bridge and device
        return board.bridge
```

**Port identification:**
- gem5's XBar already tracks packet source ports
- L3 cache has separate mem_side connections for each core
- MemBus/SystemXBar receives packets with source port metadata
- Routing decision based on: "Which L3 cache port did this packet come from?"

**Advantages of port-based routing:**
- ✅ No address space conflicts - both cores use same addresses
- ✅ Transparent to software - Linux sees unified memory
- ✅ Simple hardware logic - just check source port
- ✅ No modifications to memory maps or NUMA topology
- ✅ `taskset` naturally determines which path is taken

**Cache Coherence Handling:**
- **Both cores share L3 cache** - coherence maintained at L3 level
- gem5's coherence protocol handles private L1/L2 cache coherence
- **Potential solution if issues arise:** Tag NMP packets with custom flags to track origin

---

## Execution Strategy

### Workload Execution

**Boot and Setup:**
1. System boots Linux in Full System mode with KVM on Host CPU (Core 0)
2. Both cores are visible to Linux as a 2-core system
3. Kernel and disk image load normally

**Running Benchmark on NMP Core:**
```bash
# Inside Linux (after boot):
# Use taskset to bind benchmark to Core 1 (NMP)
taskset -c 1 /home/cxl_benchmark/pointer_chase_nmp

# Or for Host baseline:
taskset -c 0 /home/cxl_benchmark/pointer_chase_host
```

**Statistics Collection:**
```bash
# Reset stats after Linux boot
m5 exit          # Switch from KVM to TIMING
m5 resetstats    # Reset before benchmark

# Run benchmark on NMP core
taskset -c 1 /home/cxl_benchmark/pointer_chase_nmp

# Dump stats
m5 dumpstats
```

---

## Expected Results

### Performance Comparison

| Configuration | Components Bypassed | Saved Latency | Expected Total | Speedup |
|---------------|---------------------|---------------|----------------|---------|
| **Host CPU (Core 0)** | None | 0ns | 382 ns (measured) | 1× |
| **NMP CPU (Core 1) - Conservative** | CXLBridge + CXLMemory | ~77ns | ~305 ns | **1.25×** |
| **NMP CPU (Core 1) - Optimistic** | + IOBus + additional overhead | ~100-150ns | ~230-280 ns | **1.4-1.7×** |

### Latency Breakdown

**Host CPU overhead:**
- CXL Bridge: 50ns (bridge_lat) + 12ns (proto_proc_lat) = 62ns
- CXL Memory device: 15ns (proto_proc_lat)
- Additional protocol/queuing overhead: ~50-100ns
- **Total CXL overhead: ~127-177ns**

**NMP CPU (bypasses CXL components):**
- Direct memory controller access
- Eliminates: Bridge latency + Device protocol processing
- **Minimum bypass savings: 77ns**
- **Potential total savings: 100-150ns**

---

## Implementation Files to Modify

### 1. **Python Configuration** (`configs/example/gem5_library/x86-cxl-nmp-run.py`)
```python
# Add second core to processor
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    num_cores=2,  # Core 0: Host, Core 1: NMP
    isa=ISA.X86,
)

# Configure port-based routing in MemBus/SystemXBar
# Add logic to check source port and route Core 1 directly to CXLMemBar
```

### 2. **Board Configuration** (`src/python/gem5/components/boards/x86_board.py`)
```python
# Add conditional routing in _connect_things()
# Implement port-based routing logic:
#   - Check packet source port
#   - Route Core 1 packets directly to CXLMemBar
#   - Keep Core 0 packets through CXLBridge
```

### 3. **MemBus/SystemXBar Routing**
- Modify routing table to check packet source ports
- Add conditional path: if source == Core 1 → route to CXLMemBar
- Keep default path: Core 0 → CXLBridge → IOBus → CXLMemory → CXLMemBar
- Leverage gem5's existing port tracking infrastructure

---

## Potential Challenges

### 1. **Cache Coherence**
- **Issue**: Two cores with separate L1/L2 but shared L3
- **Solution**: gem5's coherence protocol should handle this automatically
- **Mitigation**: If issues arise, can tag packets with CPU ID

### 2. **Port-Based Routing Implementation**
- **Issue**: MemBus/SystemXBar needs custom routing logic
- **Solution**: Extend XBar routing to check source port metadata
- **gem5 infrastructure**: XBars already track source ports for responses

### 3. **Linux CPU Binding**
- **Issue**: Ensure benchmark actually runs on correct core
- **Solution**: Use `taskset` with verification via `/proc/cpuinfo` and `ps -eo pid,psr,comm`

### 4. **Statistics Separation**
- **Issue**: Need to distinguish Core 0 vs Core 1 statistics
- **Solution**: gem5 separates stats by CPU ID automatically (`system.processor.cores[0/1].*`)

---

## Success Criteria

- ✅ Both cores boot and are visible to Linux
- ✅ Benchmark runs on Core 1 (NMP) using `taskset`
- ✅ NMP CPU accesses bypass CXLBridge and CXLMemory device via port-based routing
- ✅ Measured latency for NMP < Host latency
- ✅ Results show 1.25-1.7× speedup (382ns → 230-305ns)
- ✅ Validates that co-locating compute with memory reduces CXL overhead

---

## Timeline

**Phase 1: Configuration** (Current)
- Add Core 1 to processor configuration
- Implement port-based routing in MemBus/SystemXBar for direct CXLMemBar access

**Phase 2: Testing**
- Boot system, verify 2 cores visible
- Test `taskset` binding to each core
- Verify routing: Core 0 → Bridge, Core 1 → Direct

**Phase 3: Measurement**
- Run Host baseline (Core 0)
- Run NMP experiment (Core 1)
- Compare statistics and verify path differences

**Phase 4: Analysis**
- Validate speedup (target: 1.25-1.7×)
- Analyze memory access paths via gem5 debug traces
- Prepare results for advisor meeting

---

## Key Advantages of This Approach

1. **Single simulation**: Both experiments in one run
2. **Fair comparison**: Same cache hierarchy, same memory addresses
3. **Realistic**: Uses FS mode with full Linux
4. **Clean separation**: Port-based routing at hardware level
5. **Transparent**: Software-agnostic, no address space changes needed
6. **Verifiable**: Can confirm CPU binding with `taskset` and separate stats per core

---

This approach gives you an apples-to-apples comparison while modeling true NMP co-location with clean hardware-level routing!
