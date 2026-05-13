# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.

"""
Standalone CXL DRAM access test for DeviceX86Board.
No host System is present in this configuration.

What this test proves:
  - DeviceX86Board.device_iobridge correctly routes device CPU loads
    and stores in the CXL range out to an external cxl_mem_bus.
  - The cxl_mem_bus carries traffic to a DRAM backing store from which
    the device CPU can read back what it wrote.
  - Linux on the device System tolerates having CXL DRAM appear as a
    Reserved E820 region, and userspace /dev/mem mmap of that region
    works under STRICT_DEVMEM.

What this test does NOT cover (separate tests address these):
  - Host and device sharing the same CXL backing.
  - CXL-Bridge-mediated host path coexisting with the device's iobridge.
  - The existing CXLNMPDevice working unchanged in the same simulation.

Topology (standalone — there is no host System anywhere in this test):

  DeviceX86Board (the only System)
    processor.cpu  →  L1/L2  →  membus  →  device_iobridge  →
                                                ↓
                                          cxl_mem_bus  (CXLMemBar)
                                                ↓
                                          cxl_memory  (DDR4)

The cxl_mem_bus and cxl_memory are constructed at top level in this
config and adopted as children of DeviceX86Board so that:
  1. They sit in the System's SimObject tree (gem5 needs every
     SimObject to have a parent).
  2. The CXL DRAM controllers' AbstractMemory._system pointers get set
     by System::constructor at src/sim/system.cc:213 — same mechanism
     that filled in inert_cxl_mem's _system after we extended
     self.memories — the same fix the boot test already applies for
     the inert_cxl_mem terminator.
"""

from m5.objects import (
    AddrRange,
    CXLMemBar,
)

from gem5.components.boards.device_x86_board import DeviceX86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory.single_channel import (
    SingleChannelDDR3_1600,
    SingleChannelDDR4_2400,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.X86)

# -----------------------------------------------------------------------------
# Memory layout
# -----------------------------------------------------------------------------
CXL_BASE = 0x100000000          # 4 GiB — same convention as the host
CXL_SIZE = "512MB"              # keep small for fast standalone iteration

# -----------------------------------------------------------------------------
# Host-board-style cache + processor for the device
# -----------------------------------------------------------------------------
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32kB", l1i_size="32kB", l2_size="512kB",
)
memory = SingleChannelDDR3_1600(size="512MB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)
for proc in processor.start:
    proc.core.usePerf = False

# -----------------------------------------------------------------------------
# Standalone CXL plumbing — built here, adopted into DeviceX86Board
# -----------------------------------------------------------------------------
cxl_memory = SingleChannelDDR4_2400(size=CXL_SIZE)
cxl_mem_range = AddrRange(CXL_BASE, size=cxl_memory.get_size())
cxl_memory.set_memory_range([cxl_mem_range])

cxl_mem_bus = CXLMemBar()
for _, port in cxl_memory.get_mem_ports():
    cxl_mem_bus.mem_side_ports = port

# -----------------------------------------------------------------------------
# Build the board, passing the CXL plumbing in
# -----------------------------------------------------------------------------
board = DeviceX86Board(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_mem_bus=cxl_mem_bus,
    cxl_mem_range=cxl_mem_range,
)

# Adopt cxl_mem_bus and cxl_memory as children of the System so they're
# in the SimObject tree (every SimObject needs a parent before
# m5.instantiate). And extend board.memories so the cxl DRAM controllers
# get their AbstractMemory._system pointer set at System construction
# (src/sim/system.cc:213 — same fix the inert_cxl_mem terminator uses).
board.cxl_mem_bus = cxl_mem_bus
board.cxl_memory = cxl_memory
cxl_abstract_mems = [
    mc.dram for mc in cxl_memory.get_memory_controllers()
]
board.memories.extend(cxl_abstract_mems)

# -----------------------------------------------------------------------------
# Workload — boot, switch to TIMING, run the CXL access test
# -----------------------------------------------------------------------------
command = (
    "m5 exit;"  # KVM -> TIMING
    + "echo '=== DeviceX86Board CXL DRAM access test ===';"
    + "/home/cxl_benchmark/device_cxl_test;"
    + "echo '=== CXL access test done ===';"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(
        local_path="/home/alfi/CXL-NMP/fs_files/vmlinux_20240920"
    ),
    disk_image=DiskImageResource(
        local_path="/home/alfi/CXL-NMP/fs_files/parsec.img"
    ),
    readfile_contents=command,
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [lambda: processor.switch()]),
    },
)

print("=" * 80)
print("DeviceX86Board standalone CXL DRAM access test")
print("=" * 80)
print(f"  Board          : DeviceX86Board + device_iobridge")
print(f"  Processor      : X86KVM -> X86Timing (KVM fast boot)")
print(f"  Local DRAM     : 512 MiB DDR3-1600")
print(f"  CXL DRAM       : {CXL_SIZE} DDR4-2400  (range {hex(CXL_BASE)})")
print(f"  CXL bus        : CXLMemBar  (NoncoherentXBar)")
print(f"  E820           : CXL range as Reserved (type 2)")
print(f"  Worker         : /home/cxl_benchmark/device_cxl_test")
print(f"  PASS string    : 'CXL DRAM access TEST PASSED'")
print("=" * 80)

simulator.run()
