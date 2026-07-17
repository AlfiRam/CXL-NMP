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
DeviceX86Board — minimal x86 Linux board for the device side of the
CXL NMP project.

This is a stripped clone of X86Board:
  - no CXLBridge, no CXLMemory, no CXLNMPDevice (those live on the
    HOST X86Board in the two-System configuration)
  - no cxl_mem_bus by default; if a `cxl_mem_bus` constructor arg is
    passed, the board adds a Bridge into it for CXL DRAM access
  - smaller local DRAM (default 512 MiB) to keep TIMING-mode Linux
    boot tractable
  - everything else (PIC, PIT, IO-APIC, CMOS, UART, IDE for root fs,
    apicbridge, IntelMP table, E820 table, SMBios) is preserved
    because Linux refuses to boot without them.

The class supports three configurations of increasing complexity,
selected by which optional kwargs are supplied:
  - standalone Linux boot (no CXL connectivity)
  - device CPU reads/writes CXL DRAM via cxl_mem_bus
  - full two-System integration with the host X86Board

CXL bridge / CXLMemory / NMP fields removed from the device-side
SouthBridge to avoid having unused PCI devices on the device's PCI
hierarchy:
  - The host's CXLNMPDevice is not visible to device-side Linux. The
    device CPU reaches CXL DRAM via a direct memory bridge, not via
    PCI.
  - C++ SouthBridge (src/dev/x86/south_bridge.{hh,cc}) only references
    pit/pic1/pic2/cmos/speaker/io_apic. Other Python-side attributes
    on the SouthBridge SimObject are extra children that the C++ side
    does not touch. We therefore declare a parallel DeviceSouthBridge
    with the same gem5 type ("SouthBridge") but no cxlmemory/nmp_device
    children.
"""

from typing import (
    List,
    Sequence,
)

from m5.objects import (
    AddrRange,
    BaseXBar,
    Bridge,
    CowDiskImage,
    IdeDisk,
    IntegrityVerifier,
    IOXBar,
    Pc,
    Port,
    RawDiskImage,
    X86E820Entry,
    X86FsLinux,
    X86IntelMPBus,
    X86IntelMPBusHierarchy,
    X86IntelMPIOAPIC,
    X86IntelMPIOIntAssignment,
    X86IntelMPProcessor,
    X86SMBiosBiosInformation,
)
from m5.objects import (
    NoncoherentXBar,
    SimpleMemory,
)
from m5.params import *
from m5.proxy import *
from m5.util.convert import toMemorySize

from ...isas import ISA
from ...resources.resource import AbstractResource
from ...utils.override import overrides
from ..cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy
from ..cachehierarchies.classic.caches.metadata import ClassicMetadataCache
from ..memory.abstract_memory_system import AbstractMemorySystem
from ..processors.abstract_processor import AbstractProcessor
from .abstract_system_board import AbstractSystemBoard
from .kernel_disk_workload import KernelDiskWorkload


# Far-away placeholder address range for the device's inert CXL devices.
# `cxlmemory.cxl_mem_range` and the dummy backing memory must agree on
# *some* address; we put both at 16 TiB + 1 to keep them out of any real
# address space the host or device kernels could ever touch.
#
# Size string is "64KiB" (binary prefix), not "64KB". gem5's
# convert.toMemorySize() in src/python/m5/util/convert.py:65-84 lists
# every prefix used by gem5 — "Ki"/"k"/"Mi"/"M"/"Gi"/"G" but no plain
# uppercase "K". `Addr`-typed params (which BAR0.size is) call
# toMemorySize() at construction; an unrecognised prefix falls through
# to int(value, 0) which then ValueErrors on the trailing letters.
# Every existing PCI BAR in src/dev/ uses the {Ki,Mi,Gi}B convention.
_INERT_CXL_BASE  = 0x100000000000  # 16 TiB
_INERT_CXL_SIZE  = "64KiB"         # tiny — never accessed


# =============================================================================
# DeviceX86Board
# =============================================================================
class DeviceX86Board(AbstractSystemBoard, KernelDiskWorkload):
    """A minimal x86 FS-Linux board for the device side of CXL NMP.

    Differences from X86Board:
      - DeviceSouthBridge instead of stock SouthBridge (no CXL devices)
      - no CXLBridge, no CXLMemory PCI device, no cxl_mem_bus
      - mem_ranges contains only local DRAM by default. When the
        `cxl_mem_range` kwarg is supplied, the CXL range is added as
        a Reserved E820 entry alongside the device_iobridge to the
        host's cxl_mem_bus.
      - max local memory raised to 512 MiB by default to keep TIMING-mode
        boot fast; X86Board's 3 GiB cap is preserved as the upper bound.
    """

    def __init__(
        self,
        clk_freq: str,
        processor: AbstractProcessor,
        memory: AbstractMemorySystem,
        cache_hierarchy: AbstractCacheHierarchy,
        # Optional CXL plumbing. When both are provided, the
        # DeviceX86Board adds a Bridge from its membus to the supplied
        # cxl_mem_bus, claiming `cxl_mem_range`. cxl_mem_range also
        # gets added to System.mem_ranges (for routing) and into the
        # E820 table as Reserved (so Linux's page allocator stays away
        # but /dev/mem can mmap it under STRICT_DEVMEM).
        # When None, nothing CXL-related is wired.
        cxl_mem_bus=None,
        cxl_mem_range: "AddrRange | None" = None,
        # Optional device-side integrity verifier on the CXL-ingress path.
        # When True (and CXL plumbing is present), an IntegrityVerifier is
        # interposed between the device membus and device_iobridge so device
        # reads of the shared CXL window traverse it. Default OFF -> the
        # existing offload path is wired exactly as before.
        cxl_integrity: bool = False,
        cxl_integrity_tree_type: str = "TimingBmt",
        cxl_integrity_tree_arity: int = 4,
        cxl_integrity_full_ranges=None,
        cxl_integrity_os_ranges=None,
        cxl_integrity_metadata_cache_size: str = "128KiB",
        cxl_integrity_metadata_cache_assoc: int = 8,
        # §6.2 range-keyed subtree handoff. When True (requires cxl_integrity),
        # the device verifier is handed AUTHORITY over a contiguous region at
        # the start of the protected cxl_os range (size cxl_handoff_size): it
        # re-roots its integrity walk for that range. Default OFF.
        cxl_handoff: bool = False,
        cxl_handoff_size: str = "1MiB",
    ) -> None:
        # Stash CXL params before super-style init so _setup_memory_ranges
        # and _setup_io_devices can see them.
        self._cxl_mem_bus = cxl_mem_bus
        self._cxl_mem_range = cxl_mem_range

        # Device-side integrity verifier configuration. The protected region
        # is the shared CXL WINDOW (CxlOnly), not device-local DRAM.
        self._cxl_integrity = cxl_integrity
        self._cxl_integrity_tree_type = cxl_integrity_tree_type
        self._cxl_integrity_tree_arity = cxl_integrity_tree_arity
        self._cxl_integrity_metadata_cache_size = cxl_integrity_metadata_cache_size
        self._cxl_integrity_metadata_cache_assoc = cxl_integrity_metadata_cache_assoc
        self._cxl_integrity_full_ranges = cxl_integrity_full_ranges
        self._cxl_integrity_os_ranges = cxl_integrity_os_ranges
        if cxl_integrity and cxl_mem_range is not None:
            # Default carve-out: protect the bottom 2 GiB of the CXL window;
            # EXCLUDE the top 16 MiB offload mailbox from `full` so it stays
            # outside the verifier (offload traffic passes through inert) and
            # so the integrity structure (top suffix of `full`) does not land
            # on the mailbox. Integrity lands in [os_end, full_end).
            base = int(cxl_mem_range.start)
            win_size = int(cxl_mem_range.size())
            full_end = base + win_size - toMemorySize("16MiB")  # drop mailbox
            if self._cxl_integrity_full_ranges is None:
                self._cxl_integrity_full_ranges = [AddrRange(base, full_end)]
            if self._cxl_integrity_os_ranges is None:
                os_end = base + toMemorySize("2GiB")
                assert os_end < full_end, (
                    "CXL integrity os range must fit below the metadata "
                    "reserve (CXL window too small for the default carve)"
                )
                self._cxl_integrity_os_ranges = [AddrRange(base, os_end)]

        # §6.2 range-keyed handoff region: place it at the START (offset 0) of
        # the protected cxl_os range, sized cxl_handoff_size and MAC-granularity
        # (64*arity) aligned. Passed to the verifier as handoff_range_*; the
        # same base/size are threaded to the offload guests' OP_HANDOFF
        # descriptor by the config. Default (off) -> 0/0 -> verifier inactive.
        self._cxl_handoff = cxl_handoff
        self._cxl_handoff_start = 0
        self._cxl_handoff_size = 0
        if cxl_handoff:
            assert cxl_integrity, "cxl_handoff requires cxl_integrity"
            assert self._cxl_integrity_os_ranges, (
                "cxl_handoff needs a protected cxl_os range"
            )
            os_base = int(self._cxl_integrity_os_ranges[0].start)
            os_size = int(self._cxl_integrity_os_ranges[0].size())
            hsize = toMemorySize(cxl_handoff_size)
            mac_gran = 64 * cxl_integrity_tree_arity
            assert hsize % mac_gran == 0, (
                f"cxl_handoff_size must be {mac_gran}-byte (MAC-granularity) "
                "aligned"
            )
            assert hsize <= os_size, (
                "cxl_handoff region exceeds the protected cxl_os range"
            )
            self._cxl_handoff_start = os_base
            self._cxl_handoff_size = hsize
        # AbstractSystemBoard inherits from System, AbstractBoard, but
        # AbstractBoard.__init__ requires a cxl_memory and is_asic
        # parameter. We bypass AbstractSystemBoard.__init__ and call
        # AbstractBoard's grandparent (System) directly so we don't have
        # to pretend we have CXL memory on the device side.
        from m5.objects import System
        System.__init__(self)

        if processor.get_isa() != ISA.X86:
            raise Exception(
                "DeviceX86Board requires an X86 processor. Got "
                f"{processor.get_isa().name}."
            )

        # Mirror AbstractBoard.__init__ minus the cxl_memory bits.
        from m5.objects import (
            SrcClockDomain,
            VoltageDomain,
        )
        self.clk_domain = SrcClockDomain()
        self.clk_domain.clock = clk_freq
        self.clk_domain.voltage_domain = VoltageDomain()

        self.processor = processor
        self.memory = memory
        self._cache_hierarchy = cache_hierarchy
        if cache_hierarchy is not None:
            self.cache_hierarchy = cache_hierarchy

        self._is_fs = None
        self._checkpoint = None
        self._connect_things_called = False

        self._setup_memory_ranges()
        self._setup_board()

    # -- AbstractBoard plumbing ------------------------------------------

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self) -> BaseXBar:
        return self.iobus

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> Sequence[Port]:
        # Include cxlmemory.dma and nmp_device.dma even though those
        # PCI devices are inert on the device System: stock
        # SouthBridge.attachIO references them, and gem5's DMA-port
        # bookkeeping expects them in the list. Same convention the
        # host's X86Board uses (x86_board.py:369-374).
        return [
            self.pc.south_bridge.ide.dma,
            self.iobus.mem_side_ports,
            self.pc.south_bridge.cxlmemory.dma,
            self.pc.south_bridge.nmp_device.dma,
        ]

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        return self.iobus.mem_side_ports

    # -- Board setup -----------------------------------------------------

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        # Stock Pc → stock SouthBridge. The SouthBridge brings
        # `cxlmemory` and `nmp_device` along as default children
        # (src/dev/x86/SouthBridge.py:84-86). They cannot be removed
        # cleanly: those are class-level child SimObject assignments,
        # not Params, and the gem5 metaclass has no working
        # subclass-overrides-parent-default-child mechanism for them
        # (NULL assignment fires AttributeError because the attrs are
        # not in `_params`; class-level NULL is filtered out by
        # `_add_cls_child` so the parent's child is still inherited via
        # the multidict fallback). The pragmatic answer is to leave the
        # children in place and terminate their unused master ports on
        # a dummy bus — the PCI devices end up on the device's PCI
        # hierarchy but Linux finds no driver for their VendorIDs and
        # ignores them.
        self.pc = Pc()
        self.workload = X86FsLinux()
        self.iobus = IOXBar()

        self._setup_io_devices()

        self.m5ops_base = 0xFFFF0000

    def _setup_io_devices(self):
        IO_address_space_base = 0x8000000000000000
        pci_config_address_space_base = 0xC000000000000000
        interrupts_address_space_base = 0xA000000000000000
        APIC_range_size = 1 << 12

        # ---- Membus → iobus bridge ----
        # Forwards CPU accesses in the I/O / PCI-config / m5ops ranges
        # out to the iobus where the platform devices live. Same ranges
        # X86Board uses, MINUS the CXL memory range (the device has no
        # CXL connectivity).
        self.bridge = Bridge(delay="50ns")
        self.bridge.mem_side_port = self.get_io_bus().cpu_side_ports
        self.bridge.cpu_side_port = (
            self.get_cache_hierarchy().get_mem_side_port()
        )
        self.bridge.ranges = [
            AddrRange(0xC0000000, 0xFFFF0000),
            AddrRange(
                IO_address_space_base, interrupts_address_space_base - 1
            ),
            AddrRange(pci_config_address_space_base, Addr.max),
        ]

        # ---- APIC bridge: iobus → membus for interrupt-space writes ----
        self.apicbridge = Bridge(delay="50ns")
        self.apicbridge.cpu_side_port = self.get_io_bus().mem_side_ports
        self.apicbridge.mem_side_port = (
            self.get_cache_hierarchy().get_cpu_side_port()
        )
        self.apicbridge.ranges = [
            AddrRange(
                interrupts_address_space_base,
                interrupts_address_space_base
                + self.get_processor().get_num_cores() * APIC_range_size
                - 1,
            )
        ]

        # ---- attachIO() — wires platform devices to iobus ----
        # This is the stock Pc.attachIO → stock SouthBridge.attachIO
        # path, which connects cxlmemory.cxl_rsp_port / cxlmemory.dma /
        # nmp_device.pio / nmp_device.dma to the iobus. Their RequestPort
        # masters (cxlmemory.mem_req_port and nmp_device.mem_port) are
        # left dangling and would fail port-binding.
        self.pc.attachIO(self.get_io_bus())

        # ---- Dummy terminator for the inert CXL devices' master ports ----
        # cxlmemory.mem_req_port and nmp_device.mem_port both want a
        # ResponsePort downstream. We satisfy port-binding by routing
        # both to a single NoncoherentXBar that fans out to a tiny
        # SimpleMemory at an unreachable physical address (16 TiB).
        # No real traffic ever flows here: Linux's PCI scan sees the
        # cxlmemory (VendorID 0x8086 / DeviceID 0x7890) and the
        # nmp_device (VendorID 0x1234 / DeviceID 0x0001), finds no
        # matching driver, and walks past. With no driver bound, no
        # MMIO writes happen, so cxlmemory.cxl_rsp_port never receives
        # requests and therefore never emits anything from
        # mem_req_port. The terminator exists purely for the
        # port-binding pass.
        self.pc.south_bridge.cxlmemory.cxl_mem_range = AddrRange(
            _INERT_CXL_BASE, size=_INERT_CXL_SIZE
        )
        self.pc.south_bridge.cxlmemory.BAR0.size = _INERT_CXL_SIZE
        self.inert_cxl_bus = NoncoherentXBar(
            frontend_latency=1, forward_latency=0,
            response_latency=1, width=16,
        )
        self.inert_cxl_bus.cpu_side_ports = (
            self.pc.south_bridge.cxlmemory.mem_req_port
        )
        self.inert_cxl_bus.cpu_side_ports = (
            self.pc.south_bridge.nmp_device.mem_port
        )
        self.inert_cxl_mem = SimpleMemory(
            range=AddrRange(_INERT_CXL_BASE, size=_INERT_CXL_SIZE),
            latency="1ns",
        )
        self.inert_cxl_bus.mem_side_ports = self.inert_cxl_mem.port

        # ---- device_iobridge to external cxl_mem_bus ----
        # Bridges device CPU memory traffic in the CXL range out to the
        # cxl_mem_bus supplied by the caller. This is structurally the
        # same as the host X86Board's `self.bridge` connecting membus
        # to iobus, but routes to a different downstream bus and only
        # claims the CXL memory range. cache_hierarchy.get_mem_side_port()
        # is the membus's mem_side_ports — a vector — so it co-exists
        # cleanly with `self.bridge`'s tap of the same port.
        if (
            self._cxl_mem_bus is not None
            and self._cxl_mem_range is not None
        ):
            self.device_iobridge = Bridge(
                delay="50ns",
                ranges=[self._cxl_mem_range],
            )
            if self._cxl_integrity:
                # Interpose the integrity verifier on the device's
                # READ-FROM-CXL (ingress) seam: device CXL reads traverse it
                # on the way out and the verified data on the way back. The
                # verifier propagates the (whole-window) CXL range up to the
                # membus, so only CXL traffic routes through it; device-local
                # DRAM reads never reach device_iobridge and so never reach
                # the verifier. Its CxlOnly ranges decide what is actually
                # verified (the offload mailbox is excluded -> passes through).
                self.cxl_verifier = IntegrityVerifier(
                    integrity_allocation_mode="CxlOnly",
                    integrity_tree_type=self._cxl_integrity_tree_type,
                    integrity_tree_arity=self._cxl_integrity_tree_arity,
                    cxl_full_ranges=self._cxl_integrity_full_ranges,
                    cxl_os_ranges=self._cxl_integrity_os_ranges,
                    # §6.2 range-keyed handoff (0/0 when disabled -> inactive).
                    handoff_range_start=self._cxl_handoff_start,
                    handoff_range_size=self._cxl_handoff_size,
                )
                self.cxl_metadata_cache = ClassicMetadataCache(
                    size=self._cxl_integrity_metadata_cache_size,
                    assoc=self._cxl_integrity_metadata_cache_assoc,
                )
                # membus -> verifier -> device_iobridge
                self.cxl_verifier.cpu_side_port = (
                    self.get_cache_hierarchy().get_mem_side_port()
                )
                self.device_iobridge.cpu_side_port = (
                    self.cxl_verifier.mem_side_port
                )
                # metadata cache hangs off the verifier's metadata ports
                self.cxl_metadata_cache.cpu_side = (
                    self.cxl_verifier.metadata_req_port
                )
                self.cxl_verifier.metadata_resp_port = (
                    self.cxl_metadata_cache.mem_side
                )
            else:
                self.device_iobridge.cpu_side_port = (
                    self.get_cache_hierarchy().get_mem_side_port()
                )
            self.device_iobridge.mem_side_port = (
                self._cxl_mem_bus.cpu_side_ports
            )

        # Add inert_cxl_mem to the System's memories vector.
        # _setup_memory_ranges already overrode the default Self.all
        # proxy by assigning self.memories = cpu_abstract_mems, which
        # leaves any AbstractMemory created later (like this one)
        # without its _system pointer set. src/sim/system.cc:213 only
        # calls memories[x]->system(this) for memories listed in the
        # vector; an AbstractMemory whose _system stays NULL trips the
        # assert at src/mem/abstract_mem.cc:156 during regStats.
        # The host's X86Board hits the exact same pattern and solves
        # it the same way: x86_board.py:218 extends self.memories
        # with the cxl_dram controllers after creating them.
        self.memories.extend([self.inert_cxl_mem])

        # ---- BIOS / SMBios ----
        self.workload.smbios_table.structures = [X86SMBiosBiosInformation()]

        # ---- IntelMP table ----
        base_entries = []
        ext_entries = []
        for i in range(self.get_processor().get_num_cores()):
            bp = X86IntelMPProcessor(
                local_apic_id=i,
                local_apic_version=0x14,
                enable=True,
                bootstrap=(i == 0),
            )
            base_entries.append(bp)
        io_apic = X86IntelMPIOAPIC(
            id=self.get_processor().get_num_cores(),
            version=0x11,
            enable=True,
            address=0xFEC00000,
        )
        self.pc.south_bridge.io_apic.apic_id = io_apic.id
        base_entries.append(io_apic)

        pci_bus = X86IntelMPBus(bus_id=0, bus_type="PCI   ")
        base_entries.append(pci_bus)
        isa_bus = X86IntelMPBus(bus_id=1, bus_type="ISA   ")
        base_entries.append(isa_bus)
        connect_busses = X86IntelMPBusHierarchy(
            bus_id=1, subtractive_decode=True, parent_bus=0
        )
        ext_entries.append(connect_busses)

        # IDE on PCI dev 4 → IOAPIC pin 16 (same as host).
        pci_dev4_inta = X86IntelMPIOIntAssignment(
            interrupt_type="INT",
            polarity="ConformPolarity",
            trigger="ConformTrigger",
            source_bus_id=0,
            source_bus_irq=0 + (4 << 2),
            dest_io_apic_id=io_apic.id,
            dest_io_apic_intin=16,
        )
        base_entries.append(pci_dev4_inta)

        def _assignISAInt(irq, apicPin):
            base_entries.append(X86IntelMPIOIntAssignment(
                interrupt_type="ExtInt",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=0,
            ))
            base_entries.append(X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=apicPin,
            ))

        _assignISAInt(0, 2)
        _assignISAInt(1, 1)
        for i in range(3, 15):
            _assignISAInt(i, i)

        self.workload.intel_mp_table.base_entries = base_entries
        self.workload.intel_mp_table.ext_entries = ext_entries

        # ---- E820 memory map ----
        # Default: only local DRAM. No CXL entry.
        entries = [
            X86E820Entry(addr=0, size="639kB", range_type=1),
            X86E820Entry(addr=0x9FC00, size="385kB", range_type=2),
            X86E820Entry(
                addr=0x100000,
                size=f"{self.mem_ranges[0].size() - 0x100000:d}B",
                range_type=1,
            ),
            # m5ops region (last 16 KiB of 32-bit address space)
            X86E820Entry(addr=0xFFFF0000, size="64kB", range_type=2),
        ]
        # When a CXL range is provided: tell Linux it is "Reserved"
        # (type 2). This keeps the page allocator out of it, but Linux
        # still recognises the addresses as legitimate I/O — and
        # STRICT_DEVMEM permits /dev/mem mmap of non-RAM regions, which
        # is the host-userspace API the device worker uses to touch
        # CXL addresses directly. Unlike the host's X86Board (which
        # uses range_type=1 to enroll CXL as system RAM through NUMA
        # node 1), we explicitly do NOT want the device's kernel to
        # think those pages are RAM.
        if self._cxl_mem_range is not None:
            entries.append(
                X86E820Entry(
                    addr=int(self._cxl_mem_range.start),
                    size=f"{self._cxl_mem_range.size():d}B",
                    range_type=2,  # Reserved
                )
            )
        self.workload.e820_table.entries = entries

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self):
        memory = self.get_memory()
        if memory.get_size() > toMemorySize("3GB"):
            raise Exception(
                "DeviceX86Board (like X86Board) caps local DRAM at 3 GiB "
                "due to the I/O hole at 0xC0000000."
            )
        data_range = AddrRange(memory.get_size())
        memory.set_memory_range([data_range])
        cpu_abstract_mems = []
        for mc in memory.get_memory_controllers():
            cpu_abstract_mems.append(mc.dram)
        self.memories = cpu_abstract_mems
        self.mem_ranges = [
            data_range,
            AddrRange(0xC0000000, size=0x100000),
        ]
        # If a CXL range was provided: include it so the System knows to route
        # those addresses (the device_iobridge will claim them).
        if self._cxl_mem_range is not None:
            self.mem_ranges.append(self._cxl_mem_range)

    # -- Disk workload ---------------------------------------------------

    @overrides(KernelDiskWorkload)
    def get_disk_device(self):
        return "/dev/hda1"

    @overrides(KernelDiskWorkload)
    def _add_disk_to_board(self, disk_image: AbstractResource):
        ide_disk = IdeDisk()
        ide_disk.driveID = "device0"
        ide_disk.image = CowDiskImage(
            child=RawDiskImage(read_only=True), read_only=False
        )
        ide_disk.image.child.image_file = disk_image.get_local_path()
        self.pc.south_bridge.ide.disks = [ide_disk]

    @overrides(KernelDiskWorkload)
    def get_default_kernel_args(self) -> List[str]:
        return [
            "earlyprintk=ttyS0",
            "console=ttyS0",
            "lpj=7999923",
            "root={root_value}",
            "disk_device={disk_device}",
        ]
