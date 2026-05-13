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
DeviceBoard — minimal stub System.

Used to validate that gem5 can host two Systems under one Root (via
the VectorParam.System "systems" field on Root). This board provides
the absolute minimum a coexisting System needs to satisfy gem5's
SimObject tree and stat-registration passes:

  - Clock and voltage domain (so ClockedObject init succeeds)
  - StubWorkload (so System construction's panic_if(!workload) check
    passes — see src/sim/system.cc:186)
  - system_port wired directly to a 64-byte SimpleMemory terminator
    (so gem5's port-binding pass doesn't fatal on an unconnected
    RequestPort)

No CPU, no caches, no real memory, no devices. The board ticks
quietly alongside whatever real workload runs in the other System.

Used by:
    configs/example/gem5_library/x86-cxl-two-system-test.py
"""

from m5.objects import (
    AddrRange,
    SimpleMemory,
    SrcClockDomain,
    System,
    VoltageDomain,
)
from m5.objects.Workload import StubWorkload


# Stub-memory range placed far above any address the host will touch.
# Used purely as a terminator for system_port; never accessed.
_STUB_RANGE = AddrRange(0x100000000000, size="64B")  # 16 TiB


class DeviceBoard(System):
    """Empty stub System for two-System scaffolding validation.

    Constructor args:
        clk_freq : device clock (default "1GHz"). Independent from the
                   host's clock — both Systems share the global event
                   queue but each can run at its own simulated frequency.
    """

    def __init__(self, clk_freq: str = "1GHz") -> None:
        super().__init__()

        self.clk_domain = SrcClockDomain()
        self.clk_domain.clock = clk_freq
        self.clk_domain.voltage_domain = VoltageDomain()

        # Satisfies sim/system.cc:186 panic_if(!workload).
        self.workload = StubWorkload()

        # system_port is a required RequestPort (sim/System.py:57);
        # gem5's port-binding pass would fatal if we left it
        # unconnected. Bind directly to a 64-byte SimpleMemory acting
        # as a terminator. No CPU exists in this System, so no
        # requests ever reach this memory.
        self.stub_mem = SimpleMemory(range=_STUB_RANGE, latency="1ns")
        self.system_port = self.stub_mem.port

        # mem_ranges must include the stub_mem's range so the System's
        # physmem bookkeeping is consistent with what
        # `memories = Self.all` auto-collects.
        self.mem_ranges = [_STUB_RANGE]
