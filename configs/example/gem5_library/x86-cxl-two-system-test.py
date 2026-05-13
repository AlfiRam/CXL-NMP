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
Smoke test for two coexisting Systems under one Root.

Goal: prove m5.instantiate() succeeds with the host X86Board AND a
minimal DeviceBoard both attached to Root, the host Linux boots
normally, prints "hello", and the simulation exits cleanly.

What this validates:
  - Root.py's new VectorParam.System("systems") correctly carries a
    second System through to gem5's C++ layer.
  - DeviceBoard (an empty stub System with stub workload +
    system_port terminator) survives construction, init(), and tick.
  - Existing CXLNMPDevice / CXL Bridge / cxl_mem_bus topology is
    untouched on the host side, so existing host-side regression
    tests still pass.

Run after this:
    build/X86/gem5.opt configs/example/gem5_library/x86-cxl-ptr-chase-test.py
to confirm the host-side regression test still passes.
"""

import argparse

import m5
import m5.ticks
from m5.objects import Root

from gem5.components.boards.device_board import DeviceBoard
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    SingleChannelDDR4_3200,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.components.processors.switchable_processor import SwitchableProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires


class TwoSystemSimulator(Simulator):
    """
    Simulator subclass that attaches an additional System to Root before
    m5.instantiate().

    The stock Simulator._instantiate() builds Root via
    ``Root(full_system=..., board=self._board)``. We replicate that flow
    and additionally pass ``systems=[device_system]`` so the DeviceBoard
    becomes a sibling of the host board under Root. This is the new
    VectorParam.System field added to src/sim/Root.py.
    """

    def __init__(self, *args, device_system, **kwargs):
        self._device_system = device_system
        super().__init__(*args, **kwargs)

    def _instantiate(self) -> None:
        if self._instantiated:
            return

        # Pre-instantiate the host board (runs _connect_things).
        self._board._pre_instantiate()

        # Build Root with BOTH the host board AND the device system.
        root = Root(
            full_system=self._full_system
            if self._full_system is not None
            else self._board.is_fullsystem(),
            board=self._board,
            systems=[self._device_system],
        )
        self._root = root

        # KVM sim_quantum handling — copied verbatim from
        # Simulator._instantiate() so KVM cores schedule correctly.
        processor = self._board.processor
        if any(core.is_kvm_core() for core in processor.get_cores()) or (
            isinstance(processor, SwitchableProcessor)
            and any(core.is_kvm_core() for core in processor._all_cores())
        ):
            m5.ticks.fixGlobalFrequency()
            root.sim_quantum = m5.ticks.fromSeconds(0.001)

        if self._board._checkpoint:
            m5.instantiate(self._board._checkpoint.as_posix())
        else:
            m5.instantiate(self._checkpoint_path)

        self._instantiated = True
        self._board._post_instantiate()


# --- Host board: replicate x86-cxl-ptr-chase-test.py setup verbatim ---

requires(isa_required=ISA.X86)

parser = argparse.ArgumentParser(
    description="Two-System smoke test."
)
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

memory = DIMM_DDR5_4400(size="3GB")
if args.is_asic == "True":
    cxl_memory = DIMM_DDR5_4400(size="8GB")
else:
    cxl_memory = SingleChannelDDR4_3200(size="8GB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)
for proc in processor.start:
    proc.core.usePerf = False

host_board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_memory,
    is_asic=(args.is_asic == "True"),
    enable_nmp=False,
)

# --- Device system: empty stub System ---
device_system = DeviceBoard(clk_freq="1GHz")

# Trivial command: switch to TIMING (Simulator handles via on_exit_event),
# print hello, exit. This proves the host Linux is fully functional and
# the second System didn't break anything.
command = (
    "m5 exit;"  # First exit: KVM -> TIMING switch
    + "echo '=== two-System smoke test: two Systems under one Root ===';"
    + "echo hello;"
    + "echo '=== two-System smoke OK ===';"
    + "m5 exit;"
)

host_board.set_kernel_disk_workload(
    kernel=KernelResource(
        local_path="/home/alfi/CXL-NMP/fs_files/vmlinux_20240920"
    ),
    disk_image=DiskImageResource(
        local_path="/home/alfi/CXL-NMP/fs_files/parsec.img"
    ),
    readfile_contents=command,
)

simulator = TwoSystemSimulator(
    board=host_board,
    device_system=device_system,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [lambda: processor.switch()])
    },
)

print("=" * 80)
print("Two-System Smoke Test")
print("=" * 80)
print(f"  Host board:     X86Board (ptr-chase-test config)")
print(f"  Device system:  DeviceBoard (empty stub, clk=1GHz)")
print(f"  Root.systems:   [device_system]   (new VectorParam.System)")
print(f"  Expected:       host Linux boots, prints 'hello', exits cleanly")
print("=" * 80)

simulator.run()
