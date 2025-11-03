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
Host Baseline Configuration for CXL-DMSim

This script runs a single-core baseline configuration where the CPU accesses
CXL memory through the normal path:

- Core 0 (Host CPU): Accesses CXL memory through normal path
  (L3 → MemBus → CXL Bridge → IOBus → CXL Memory device → CXL Memory Bus)
  Expected latency: ~382ns per access

This serves as the baseline for comparison with the NMP configuration.

Usage
-----

```
scons build/X86/gem5.opt -j16
build/X86/gem5.opt -d m5out_host configs/example/gem5_library/x86-cxl-host-run.py
```
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

parser = argparse.ArgumentParser(description="CXL NMP system parameters.")
parser.add_argument(
    "--is_asic",
    action="store",
    type=str,
    nargs="?",
    choices=["True", "False"],
    default="True",
    help="Choose to simulate CXL ASIC Device or FPGA Device.",
)
parser.add_argument(
    "--test_cmd",
    type=str,
    choices=[
        "lmbench_cxl.sh",
        "lmbench_dram.sh",
        "merci_dram.sh",
        "merci_cxl.sh",
        "merci_dram+cxl.sh",
        "stream_dram.sh",
        "stream_cxl.sh",
        "pointer_chase",
        "memory_stride_access",
    ],
    default="memory_stride_access",
    help="Choose a test to run.",
)
parser.add_argument(
    "--cpu_type",
    type=str,
    choices=["TIMING", "O3"],
    default="TIMING",
    help="CPU type",
)
parser.add_argument(
    "--cxl_mem_type",
    type=str,
    choices=["Simple", "DRAM"],
    default="DRAM",
    help="CXL memory type",
)

args = parser.parse_args()

# Here we setup a standard MESI Three Level Cache Hierarchy for the host baseline.
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

# Here we setup the processor with a single core for baseline measurement
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,  # Use KVM for fast boot
    switch_core_type=CPUTypes.O3 if args.cpu_type == "O3" else CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,  # Single core baseline
)

# Disable perf for KVM cores
for proc in processor.start:
    proc.core.usePerf = False

# Here we setup the board WITHOUT NMP (standard CXL path)
board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_memory,
    is_asic=(args.is_asic == "True"),
    enable_nmp=False,  # Standard CXL path (baseline)
)

# This is the command to run after the system has booted.
# The first `m5 exit` switches from KVM to TIMING/O3 cores.
# Then run the benchmark on the single host core (normal CXL path).
command = (
    "m5 exit;"
    + "echo '';"
    + "echo '=== Host Baseline (Single Core, Normal CXL Path) ===';"
    + "echo 'Core: 1 CPU (Core 0)';"
    + "echo 'Path: L3 → MemBus → CXLBridge → IOBus → CXLMemory → CXL_Mem_Bus';"
    + "echo 'Expected latency: ~382ns per access';"
    + "echo '';"
    + "NMP_CORE=0 /home/cxl_benchmark/"
    + args.test_cmd
    + ";"
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

simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: (func() for func in [processor.switch])},
)

print("=" * 80)
print("CXL-DMSim Host Baseline Simulation")
print("=" * 80)
print(f"Configuration:")
print(f"  - Cores: 1 (Host CPU only)")
print(f"  - Boot CPU: KVM (fast boot)")
print(f"  - Runtime CPU: {args.cpu_type}")
print(f"  - CXL Device: {'ASIC' if args.is_asic == 'True' else 'FPGA'}")
print(f"  - Local Memory: 3GB DDR5-4400")
print(
    f"  - CXL Memory: 8GB {'DDR5-4400' if args.is_asic == 'True' else 'DDR4-3200'}"
)
print(f"  - NMP Routing: DISABLED (Standard CXL path)")
print(f"")
print(f"Routing Path:")
print(
    f"  Core 0: L3 → MemBus → CXLBridge (62ns) → CXLMemory (15ns) → CXL Memory"
)
print(f"")
print(
    f"Expected Result: ~382ns latency per access (baseline for NMP comparison)"
)
print("=" * 80)
print("")

# m5.stats.reset()  # Don't reset here, let command script control stats

simulator.run()
