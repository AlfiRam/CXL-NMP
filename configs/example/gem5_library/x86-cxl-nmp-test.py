# Copyright (c) 2021 The Regents of the University of California
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
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
CXL NMP Device Test Configuration - Phase 2 DMA Engine Verification

This script tests the CXLNMPDevice Phase 2 DMA engine implementation.

System Configuration:
- CXLNMPDevice: PCI device at 00:07.0 (added unconditionally in SouthBridge.py)
  - PIO interface (BAR2): 8 × 64-bit registers for control
  - Memory port: Direct connection to cxl_mem_bus (bypasses CXL Bridge overhead)

- enable_nmp=False: The OLD Core 1 bypass is disabled (not needed for testing new device)

Test Program (nmp_device_test.c):
1. Discovers NMP device BAR2 address from sysfs
2. Maps BAR2 for register access (PIO path: CPU → CXL Bridge → IO Bus → NMP)
3. Allocates source/dest buffers in CXL memory (0x100000000+)
4. Programs NMP device registers: INPUT_ADDR, OUTPUT_ADDR, DATA_SIZE, OPCODE
5. Starts DMA operation (writes CONTROL = START)
6. Polls STATUS register until DONE
7. Verifies source and destination buffers match

Expected Behavior:
- NMP device reads from CXL DRAM via mem_port (direct to cxl_mem_bus)
- Performs memcpy operation (Phase 2: simple copy)
- Writes to CXL DRAM via mem_port (direct to cxl_mem_bus)
- Sets STATUS = DONE
- Test program reports PASS

Success Criteria:
1. Test program output: "TEST PASSED"
2. Debug trace (--debug-flags=CXLNMPDevice): Shows DMA operation state machine
3. Stats: board.pc.south_bridge.nmp_device.mem_port shows read/write traffic
4. Stats: CXL Bridge does NOT show additional traffic (proves bypass works)

Usage:
    scons build/X86/gem5.opt -j16
    build/X86/gem5.opt \\
        --debug-flags=CXLNMPDevice \\
        -d m5out_nmp_test \\
        configs/example/gem5_library/x86-cxl-nmp-test.py
"""
import argparse

import m5

from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    SingleChannelDDR4_3200,
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

# This runs a check to ensure the gem5 binary is compiled to X86 and to the
# MESI Three Level coherence protocol.
requires(
    isa_required=ISA.X86,
)
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)

parser = argparse.ArgumentParser(description="CXL NMP Device Test parameters.")
parser.add_argument(
    "--is_asic",
    action="store",
    type=str,
    nargs="?",
    choices=["True", "False"],
    default="True",
    help="Choose to simulate CXL ASIC Device or FPGA Device.",
)

args = parser.parse_args()

# Here we setup a standard MESI Three Level Cache Hierarchy
cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)

# Setup the system memory.
memory = DIMM_DDR5_4400(size="3GB")
if args.is_asic:
    cxl_memory = DIMM_DDR5_4400(size="8GB")
else:
    cxl_memory = SingleChannelDDR4_3200(size="8GB")

# Here we setup the processor with a single core
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,  # Use KVM for fast boot
    switch_core_type=CPUTypes.TIMING,  # Switch to TIMING for test
    isa=ISA.X86,
    num_cores=1,  # Single core
)

# Disable perf for KVM cores
for proc in processor.start:
    proc.core.usePerf = False

# Setup board WITHOUT old Core 1 bypass (enable_nmp=False)
# The new CXLNMPDevice is added unconditionally in SouthBridge.py
board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_memory,
    is_asic=(args.is_asic == "True"),
    enable_nmp=False,  # OLD bypass disabled - not needed for new NMP device
)

# Command to run after boot
command = (
    "m5 exit;"  # Exit KVM, switch to TIMING
    + "echo '';"
    + "echo '=== CXL NMP Device Test (Phase 2 DMA Engine) ===';"
    + "echo 'Device: PCI 00:07.0 (CXLNMPDevice)';"
    + "echo 'Test: nmp_device_test.c';"
    + "echo '';"
    + "echo '[PCI ENUMERATION] Checking NMP device:';"
    + "lspci -v -s 00:07.0;"
    + "echo '';"
    + "echo '[BAR2 ADDRESS] Checking BAR2 assignment:';"
    + "cat /sys/bus/pci/devices/0000:00:07.0/resource | head -3;"
    + "echo '';"
    + "echo '[RUNNING TEST] Executing nmp_device_test...';"
    + "echo 'Using numactl --membind=1 to allocate from CXL NUMA node';"
    + "echo '';"
    + "numactl --membind=1 /home/cxl_benchmark/nmp_device_test;"
    + "echo '';"
    + "m5 exit;"
)

# Kernel and disk image paths
board.set_kernel_disk_workload(
    kernel=KernelResource(
        local_path="/home/malfiram/CXL-DMSim/fs_files/vmlinux_20240920"
    ),
    disk_image=DiskImageResource(
        local_path="/home/malfiram/CXL-DMSim/fs_files/parsec.img"
    ),
    readfile_contents=command,
)

# Set up exit event to handle CPU switch
simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (
            func() for func in [lambda: processor.switch()]
        )  # Switch to TIMING on first exit
    },
)

print("=" * 80)
print("CXL NMP Device Test Configuration (Phase 2)")
print("=" * 80)
print(f"Configuration:")
print(f"  - Cores: 1 (Host CPU)")
print(f"  - CPU Mode: KVM → TIMING (for test accuracy)")
print(f"  - CXL Device: {'ASIC' if args.is_asic == 'True' else 'FPGA'}")
print(f"  - Local Memory: 3GB DDR5-4400")
print(
    f"  - CXL Memory: 8GB {'DDR5-4400' if args.is_asic == 'True' else 'DDR4-3200'}"
)
print(f"  - NMP Device: Enabled (Phase 2 DMA engine)")
print(f"  - Old Core 1 Bypass: Disabled (enable_nmp=False)")
print(f"")
print(f"Test: nmp_device_test.c")
print(f"  - Discovers BAR2 address via sysfs")
print(f"  - Maps BAR2 for register access")
print(f"  - Allocates CXL memory buffers (4KB)")
print(f"  - Programs NMP device for memcpy")
print(f"  - Verifies DMA operation")
print(f"")
print(f"Expected Result: TEST PASSED")
print(f"Debug: Use --debug-flags=CXLNMPDevice to see DMA state machine")
print("=" * 80)
print("")

simulator.run()
