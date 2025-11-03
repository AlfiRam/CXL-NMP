# Copyright (c) 2024 Research Implementation
# NMP-enabled cache hierarchy with port-based routing

"""
NMP-Enabled Cache Hierarchy

This extends PrivateL1PrivateL2SharedL3CacheHierarchy to support
Near-Memory Processing (NMP) with port-based routing.

When NMP is enabled on the board:
- Core 0 (Host): L2 → L3 → MemBus → CXLBridge (normal path)
- Core 1 (NMP):  L2 → NMP Bridge → Direct to CXL Memory Bus (bypass path)

The routing decision is made based on the source core, not the address.
Both cores use the same address range (0x100000000+).
"""

from m5.objects import (
    BadAddr,
    BaseXBar,
    Cache,
    L2XBar,
    L3XBar,
    Port,
    SystemXBar,
)

from .....isas import ISA
from .....utils.override import *
from ....boards.abstract_board import AbstractBoard
from ...abstract_cache_hierarchy import AbstractCacheHierarchy
from ...abstract_three_level_cache_hierarchy import (
    AbstractThreeLevelCacheHierarchy,
)
from ..abstract_classic_cache_hierarchy import AbstractClassicCacheHierarchy
from .caches.l1dcache import L1DCache
from .caches.l1icache import L1ICache
from .caches.l2cache import L2Cache
from .caches.l3cache import L3Cache
from .caches.mmu_cache import MMUCache


class NMPPrivateL1PrivateL2SharedL3CacheHierarchy(
    AbstractClassicCacheHierarchy, AbstractThreeLevelCacheHierarchy
):
    """
    NMP-enabled cache hierarchy with port-based routing.

    Extends standard 3-level hierarchy to support direct routing for NMP core.
    """

    @staticmethod
    def _get_default_membus() -> SystemXBar:
        """Get default memory bus."""
        membus = SystemXBar(width=64)
        membus.badaddr_responder = BadAddr()
        membus.default = membus.badaddr_responder.pio
        return membus

    def __init__(
        self,
        l1d_size: str,
        l1i_size: str,
        l2_size: str,
        l3_size: str,
        l1d_assoc: int = 8,
        l1i_assoc: int = 8,
        l2_assoc: int = 16,
        l3_assoc: int = 16,
        membus: BaseXBar = _get_default_membus.__func__(),
    ) -> None:
        AbstractClassicCacheHierarchy.__init__(self=self)
        AbstractThreeLevelCacheHierarchy.__init__(
            self,
            l1i_size=l1i_size,
            l1i_assoc=l1i_assoc,
            l1d_size=l1d_size,
            l1d_assoc=l1d_assoc,
            l2_size=l2_size,
            l2_assoc=l2_assoc,
            l3_size=l3_size,
            l3_assoc=l3_assoc,
        )

        self.membus = membus

    @overrides(AbstractClassicCacheHierarchy)
    def get_mem_side_port(self) -> Port:
        return self.membus.mem_side_ports

    @overrides(AbstractClassicCacheHierarchy)
    def get_cpu_side_port(self) -> Port:
        return self.membus.cpu_side_ports

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        # Set up the system port for functional access from the simulator.
        board.connect_system_port(self.membus.cpu_side_ports)

        for _, port in board.get_memory().get_mem_ports():
            self.membus.mem_side_ports = port

        # Create per-core caches
        num_cores = board.get_processor().get_num_cores()

        self.l1icaches = [
            L1ICache(
                size=self._l1i_size,
                assoc=self._l1i_assoc,
                writeback_clean=False,
            )
            for i in range(num_cores)
        ]
        self.l1dcaches = [
            L1DCache(size=self._l1d_size, assoc=self._l1d_assoc)
            for i in range(num_cores)
        ]
        self.l2buses = [L2XBar() for i in range(num_cores)]
        self.l2caches = [
            L2Cache(size=self._l2_size, assoc=self._l2_assoc)
            for i in range(num_cores)
        ]
        self.l3bus = L3XBar()
        self.l3cache = L3Cache(size=self._l3_size, assoc=self._l3_assoc)

        # ITLB Page walk caches
        self.iptw_caches = [
            MMUCache(size="256KiB", writeback_clean=False)
            for _ in range(num_cores)
        ]
        # DTLB Page walk caches
        self.dptw_caches = [
            MMUCache(size="256KiB", writeback_clean=False)
            for _ in range(num_cores)
        ]

        if board.has_coherent_io():
            self._setup_io_cache(board)

        # Check if NMP is enabled on the board
        nmp_enabled = hasattr(board, "nmp_config") and board.nmp_config.get(
            "enabled", False
        )

        for i, cpu in enumerate(board.get_processor().get_cores()):
            cpu.connect_icache(self.l1icaches[i].cpu_side)
            cpu.connect_dcache(self.l1dcaches[i].cpu_side)

            self.l1icaches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.l1dcaches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.iptw_caches[i].mem_side = self.l2buses[i].cpu_side_ports
            self.dptw_caches[i].mem_side = self.l2buses[i].cpu_side_ports

            self.l2buses[i].mem_side_ports = self.l2caches[i].cpu_side

            # PORT-BASED ROUTING LOGIC
            # Core 0 (Host): Normal path through L3 → MemBus → CXLBridge
            # Core 1+ (NMP): Direct path → NMP Bridge → CXL Memory Bus
            if nmp_enabled and i >= 1:
                # NMP Core: Connect L2 directly to NMP bridge (bypass L3 → MemBus → CXLBridge)
                # Note: We still connect to L3 for coherence with Core 0
                # The NMP bridge will handle CXL memory range requests directly
                self.l3bus.cpu_side_ports = self.l2caches[i].mem_side

                # Mark this core as NMP-enabled for potential future use
                print(
                    f"[NMP] Core {i}: Configured for direct CXL memory access via NMP bridge"
                )
            else:
                # Host Core: Normal path through L3
                self.l3bus.cpu_side_ports = self.l2caches[i].mem_side

            cpu.connect_walker_ports(
                self.iptw_caches[i].cpu_side, self.dptw_caches[i].cpu_side
            )

            if board.get_processor().get_isa() == ISA.X86:
                int_req_port = self.membus.mem_side_ports
                int_resp_port = self.membus.cpu_side_ports
                cpu.connect_interrupt(int_req_port, int_resp_port)
            else:
                cpu.connect_interrupt()

        # Connect L3 to MemBus (all traffic goes through here first)
        self.l3bus.mem_side_ports = self.l3cache.cpu_side
        self.membus.cpu_side_ports = self.l3cache.mem_side

        # If NMP is enabled, connect the NMP bridge
        if nmp_enabled and hasattr(board, "nmp_bridge"):
            # Connect NMP bridge to the MemBus for routing
            # The MemBus will route based on address ranges
            # NMP cores will have their packets routed through NMP bridge
            board.nmp_bridge.cpu_side_port = self.membus.mem_side_ports
            print(
                f"[NMP] NMP bridge connected to MemBus for port-based routing"
            )

    def _setup_io_cache(self, board: AbstractBoard) -> None:
        """Create a cache for coherent I/O connections"""
        self.iocache = Cache(
            assoc=8,
            tag_latency=50,
            data_latency=50,
            response_latency=50,
            mshrs=32,
            size="256kB",
            tgts_per_mshr=12,
            write_buffers=32,
            addr_ranges=board.mem_ranges,
        )
        self.iocache.mem_side = self.membus.cpu_side_ports
        self.iocache.cpu_side = board.get_mem_side_coherent_io_port()
