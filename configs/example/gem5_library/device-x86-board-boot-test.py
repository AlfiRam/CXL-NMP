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
Standalone boot test for DeviceX86Board.

Goal: prove the stripped x86 board (no CXL Bridge, no CXLMemory PCI
device, no NMP, no cxl_mem_bus) can boot Linux and reach userspace
using the existing host vmlinux + parsec.img.

There is no second System in this test, no Root.systems vector, no
iobridge to cxl_mem_bus — just DeviceX86Board, alone, under a single
Root.

PASS criteria:
    Guest serial output contains "device boot OK".

If this fails, the failure is isolated to the stripped board itself
(DeviceSouthBridge omissions, IntelMP table mismatch, E820, kernel
cmdline) — not to two-System wiring or the iobridge.
"""

from gem5.components.boards.device_x86_board import DeviceX86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
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

# Minimal cache hierarchy — L1 caches required (NoCache panics on FS,
# documented at src/python/gem5/components/cachehierarchies/classic/
# no_cache.py:50-63). Two-level (L1+L2) chosen over three-level to keep
# the simobj count down and TIMING boot a little faster.
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32kB",
    l1i_size="32kB",
    l2_size="512kB",
)

# 512 MiB local DRAM. Plenty for a Linux init + a small userspace; small
# enough that the kernel's mem_init isn't the boot bottleneck.
memory = SingleChannelDDR3_1600(size="512MB")

# KVM → TIMING fast boot.
# A TIMING-only boot of this board burns ~25 min per iteration, which
# is unworkable for repeated debug cycles. Use the same
# SimpleSwitchableProcessor pattern the host's ptr-chase test uses
# (configs/example/gem5_library/x86-cxl-ptr-chase-test.py:131-138):
# boot under KVM (sec), `m5 exit` triggers processor.switch() in the
# on_exit_event handler, sim continues under TIMING for the actual
# verification work.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)
for proc in processor.start:
    proc.core.usePerf = False

board = DeviceX86Board(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# First m5 exit triggers KVM→TIMING switch. Then echo the PASS string
# under TIMING and exit cleanly.
command = (
    "m5 exit;"  # KVM -> TIMING switch
    + "echo '=== DeviceX86Board boot test ===';"
    + "echo 'device boot OK';"
    + "echo '=== boot test done ===';"
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
print("DeviceX86Board standalone Linux boot (KVM fast-boot)")
print("=" * 80)
print("  Board       : DeviceX86Board (no CXL, no NMP, no iobridge)")
print("  Processor   : 1x X86KVM -> X86Timing @ 1 GHz")
print("  Memory      : 512 MiB DDR3-1600  (local DRAM only)")
print("  Cache       : Private L1 + Private L2")
print("  Workload    : X86FsLinux + vmlinux_20240920 + parsec.img")
print("  Mode        : KVM (boot) -> TIMING (after first m5 exit)")
print("  Wall-clock  : ~30 sec total")
print("  PASS string : 'device boot OK'")
print("=" * 80)

simulator.run()
