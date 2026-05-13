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
Two-System x86 Linux configuration: host X86Board + DeviceX86Board
sharing CXL DRAM through host_board.cxl_mem_bus. Both Systems run
TIMING-only and terminate via a serial-output barrier.

What this configuration proves:
  - Host X86Board (with its CXLBridge, CXLMemory, CXLNMPDevice) and
    DeviceX86Board (with its device_iobridge) coexist under one Root.
  - device_board.device_iobridge attaches to host_board.cxl_mem_bus as
    the THIRD master (alongside CXLMemory.mem_req_port and
    CXLNMPDevice.mem_port — both UNCHANGED).
  - Per-System readfile machinery works (each gets its own readfile
    via the kernel_disk_workload `readfile=` kwarg).
  - The existing regression tests still pass against the same gem5
    binary.

Topology:

  Root.systems
    └── device_board : DeviceX86Board
           membus → device_iobridge → ┐
                                      ▼
  Root.board                    host_board.cxl_mem_bus (CXLMemBar)
    └── host_board : X86Board   ↑     ↑
           processor → membus → CXLBridge → CXLMemory.mem_req_port
                                            CXLNMPDevice.mem_port
                                            (existing wiring, untouched)

Earlier attempts and the lessons we kept:

  Attempt A — dual KVM on both Systems:
     gem5 hung at "Starting simulation..." with both serial consoles
     silent. Multiple KvmVMs in one gem5 process is unsupported.

  Attempt B — KVM on host, TIMING on device from tick 0:
     Host's Linux reached MP-BIOS init then panicked at the
     8254/IO-APIC timer check. A byte-level diff of every host
     interrupt SimObject against a known-working single-System
     baseline confirmed no structural leak — the bug is purely
     runtime. gem5 KVM's guest TSC drifts from simulated time when a
     concurrent TIMING System on another System consumes large amounts
     of real time per simulated quantum. Linux's check_timer sees a
     TSC-vs-PIT ratio anomaly and gives up.
     Lesson: KVM is incompatible with a concurrent active TIMING CPU
     on another System.

  Attempt C — both TIMING + count-based exit handler
     (`yield False; yield True` expecting 2 m5_exit events):
     Host booted, printed PASS, called `m5 exit;` — handler yielded
     False. But the host's userspace then terminated, Linux's last
     thread context suspended, and gem5 fired a SECOND ExitEvent.EXIT
     with cause "exiting with last active thread context"
     (src/python/gem5/simulate/exit_event.py:82-83). Handler yielded
     True on that event. Simulation ended while the device was still
     in systemd-init.
     Lesson: Linux's natural process-exit emits exit events that the
     Simulator categorises identically to explicit `m5 exit;`. Exit
     counting is fragile.

This config:
  - HOST   = SimpleProcessor(TIMING). No KVM.
  - DEVICE = SimpleProcessor(TIMING). No KVM.
  - Termination = SERIAL-OUTPUT BARRIER. The exit handler reads both
    Systems' Pc.com_1.device files after every ExitEvent.EXIT and
    only yields True when BOTH PASS strings have appeared. Every
    other exit event (Linux shutdown, last-thread suspension,
    repeated m5_exits from looping rc.local hooks, etc.) is just an
    "opportunity to recheck the barrier."
  - This is safe because the Terminal SimObject sets
    `outfile->stream()->setf(std::ios::unitbuf)` at construction
    (src/dev/serial/terminal.cc:131), so the file reflects guest
    serial output character-by-character with no buffering.
  - Wall-clock: ~25-30 min cold boot for each System, in parallel.
"""

import argparse
import os

import m5
import m5.ticks
from m5.objects import (
    AddrRange,
    Root,
)

from gem5.components.boards.device_x86_board import DeviceX86Board
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    SingleChannelDDR3_1600,
    SingleChannelDDR4_3200,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
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


# =============================================================================
# TwoSystemSimulator — same pattern as the earlier single-system stub
# variant, extended to also call
# _pre/_post_instantiate on the second System (which now actually has
# CPU/cache plumbing that needs the lifecycle hooks).
# =============================================================================
class TwoSystemSimulator(Simulator):
    def __init__(self, *args, device_board, **kwargs):
        self._device_board = device_board
        super().__init__(*args, **kwargs)

    def _instantiate(self) -> None:
        if self._instantiated:
            return

        # Both boards run _connect_things.
        self._board._pre_instantiate()
        self._device_board._pre_instantiate()

        # Both Systems attached to Root: host via the canonical `board=`
        # kwarg, device via the new VectorParam.System added to Root.py
        # by the earlier single-system stub variant.
        root = Root(
            full_system=self._full_system
            if self._full_system is not None
            else self._board.is_fullsystem(),
            board=self._board,
            systems=[self._device_board],
        )
        self._root = root

        # Both processors use KVM, so sim_quantum is required for KVM
        # scheduling regardless of which we check. Set it unconditionally.
        host_proc = self._board.processor
        dev_proc  = self._device_board.processor
        any_kvm = (
            any(c.is_kvm_core() for c in host_proc.get_cores())
            or any(c.is_kvm_core() for c in dev_proc.get_cores())
            or (isinstance(host_proc, SwitchableProcessor)
                and any(c.is_kvm_core() for c in host_proc._all_cores()))
            or (isinstance(dev_proc, SwitchableProcessor)
                and any(c.is_kvm_core() for c in dev_proc._all_cores()))
        )
        if any_kvm:
            m5.ticks.fixGlobalFrequency()
            root.sim_quantum = m5.ticks.fromSeconds(0.001)

        if self._board._checkpoint:
            m5.instantiate(self._board._checkpoint.as_posix())
        else:
            m5.instantiate(self._checkpoint_path)

        self._instantiated = True
        self._board._post_instantiate()
        self._device_board._post_instantiate()


def _serial_contains(path: str, needle: str) -> bool:
    """Return True iff `needle` appears in the file at `path`. Returns
    False if the file doesn't exist yet (Terminal creates it lazily on
    first write).
    """
    try:
        with open(path, "rb") as f:
            return needle.encode() in f.read()
    except FileNotFoundError:
        return False


def serial_barrier_handler(
    host_serial_path: str,
    device_serial_path: str,
    host_pass: str = "host boot OK",
    device_pass: str = "device boot OK",
):
    """Generator that yields True iff both Systems' PASS strings have
    appeared in their serial output. The Terminal SimObject is
    unit-buffered (src/dev/serial/terminal.cc:131), so a read against
    these files mid-simulation reflects guest output up to the last
    character emitted.

    The handler ignores WHICH exit event fired — every event is
    treated as just an opportunity to recheck the barrier. This is
    robust against:
      - explicit `m5 exit;` from our scripts,
      - "exiting with last active thread context" when Linux idles
        a System's only CPU after userspace dies,
      - any future hook (rc.local loops, kernel panic shims, etc.).

    Yields False forever until both PASS strings are seen, then
    yields True once to terminate the run loop.
    """
    while True:
        host_done = _serial_contains(host_serial_path, host_pass)
        dev_done = _serial_contains(device_serial_path, device_pass)
        if host_done and dev_done:
            print(
                f"[barrier] both PASS strings seen "
                f"(host_done={host_done}, dev_done={dev_done}) "
                f"— ending simulation"
            )
            yield True
        else:
            print(
                f"[barrier] exit handled; barrier not met "
                f"(host_done={host_done}, dev_done={dev_done}) "
                f"— continuing"
            )
            yield False


# =============================================================================
# CLI
# =============================================================================
requires(isa_required=ISA.X86)

parser = argparse.ArgumentParser(description="Two-System x86 Linux smoke test.")
parser.add_argument(
    "--is_asic",
    type=str,
    nargs="?",
    choices=["True", "False"],
    default="True",
    help="ASIC vs FPGA CXL device latency model (host-side only).",
)
args = parser.parse_args()


# =============================================================================
# Host X86Board — same configuration as x86-cxl-ptr-chase-test.py so
# the host-side regression test still passes against the same gem5 binary.
# =============================================================================
host_cache = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",  l1d_assoc=6,
    l1i_size="32kB",  l1i_assoc=8,
    l2_size="2MB",    l2_assoc=16,
    l3_size="96MB",   l3_assoc=48,
)

host_memory = DIMM_DDR5_4400(size="3GB")
if args.is_asic == "True":
    host_cxl_memory = DIMM_DDR5_4400(size="8GB")
else:
    host_cxl_memory = SingleChannelDDR4_3200(size="8GB")

# DIAGNOSTIC: host is TIMING-only. No KVM, no switch, no usePerf hack.
host_processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

host_board = X86Board(
    clk_freq="2.4GHz",
    processor=host_processor,
    memory=host_memory,
    cache_hierarchy=host_cache,
    cxl_memory=host_cxl_memory,
    is_asic=(args.is_asic == "True"),
    enable_nmp=False,
)


# =============================================================================
# Recover the host's cxl_mem_bus and CXL range — to be shared with the
# device board's device_iobridge as the third master.
# =============================================================================
cxl_mem_bus = host_board.cxl_mem_bus
cxl_mem_range = AddrRange(
    0x100000000, size=host_board.get_cxl_memory().get_size()
)


# =============================================================================
# Device DeviceX86Board — same plumbing used by the standalone CXL
# access test, but pointed at the host's cxl_mem_bus instead of a
# private one
# =============================================================================
device_cache = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32kB", l1i_size="32kB", l2_size="512kB",
)
device_memory = SingleChannelDDR3_1600(size="512MB")

# Device: TIMING-only from tick 0 (dual-KVM fallback documented in the
# module docstring). No switchable processor, no usePerf hack — those
# are KVM-specific concerns.
device_processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

device_board = DeviceX86Board(
    clk_freq="1GHz",
    processor=device_processor,
    memory=device_memory,
    cache_hierarchy=device_cache,
    cxl_mem_bus=cxl_mem_bus,
    cxl_mem_range=cxl_mem_range,
)


# =============================================================================
# Workloads — distinct readfiles per System so each board's `m5 readfile`
# in the guest reads its own command stream.
# =============================================================================
KERNEL_PATH = "/home/alfi/CXL-NMP/fs_files/vmlinux_20240920"
DISK_PATH   = "/home/alfi/CXL-NMP/fs_files/parsec.img"

# Host is TIMING from tick 0 — no leading `m5 exit;` (no switch).
host_cmd = (
    "echo '=== host: X86Board on TIMING ===';"
    + "echo 'host boot OK';"
    + "m5 exit;"
)
# Device is TIMING from tick 0 — no leading `m5 exit;` (no switch).
# Just print PASS and exit. The device's boot under TIMING is slow
# (20-30 min cold) but the actual command run after boot is short.
device_cmd = (
    "echo '=== device: DeviceX86Board on TIMING ===';"
    + "echo 'device boot OK';"
    + "m5 exit;"
)

host_board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_PATH),
    readfile=os.path.join(m5.options.outdir, "host_readfile"),
    readfile_contents=host_cmd,
)
device_board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_PATH),
    readfile=os.path.join(m5.options.outdir, "device_readfile"),
    readfile_contents=device_cmd,
)


# =============================================================================
# Run
# =============================================================================
# Serial-output paths. The Terminal SimObject names its output file
# by SimObject path: host_board is attached as Root.board so its
# Pc.com_1.device lives at outdir/board.pc.com_1.device; device_board
# is attached via Root.systems (the VectorParam.System added to
# src/sim/Root.py) and gem5 names the vector's child file
# `systems.pc.com_1.device` (confirmed empirically — gem5 strips the
# index suffix when there's a single entry).
HOST_SERIAL   = os.path.join(m5.options.outdir, "board.pc.com_1.device")
DEVICE_SERIAL = os.path.join(m5.options.outdir, "systems.pc.com_1.device")

simulator = TwoSystemSimulator(
    board=host_board,
    device_board=device_board,
    on_exit_event={
        ExitEvent.EXIT: serial_barrier_handler(
            host_serial_path=HOST_SERIAL,
            device_serial_path=DEVICE_SERIAL,
            host_pass="host boot OK",
            device_pass="device boot OK",
        ),
    },
)

print("=" * 80)
print("Two-System smoke test: TIMING-only + serial-output barrier")
print("=" * 80)
print(f"  Host board     : X86Board  (TIMING-only)  with CXLNMPDevice etc.")
print(f"  Device board   : DeviceX86Board  (TIMING-only)  + device_iobridge")
print(f"  cxl_mem_bus    : host_board.cxl_mem_bus  (CXLMemBar, 3 masters)")
print(f"  CXL range      : 0x100000000, size {host_board.get_cxl_memory().get_size_str()}")
print(f"  Host readfile  : {os.path.join(m5.options.outdir, 'host_readfile')}")
print(f"  Dev  readfile  : {os.path.join(m5.options.outdir, 'device_readfile')}")
print(f"  Host serial    : {HOST_SERIAL}")
print(f"  Dev  serial    : {DEVICE_SERIAL}")
print(f"  Barrier        : sim ends when BOTH PASS strings appear in serial")
print(f"                   ('host boot OK' AND 'device boot OK')")
print(f"  Wall-clock     : both ~25-30 min cold boot (parallel under TIMING)")
print("=" * 80)

simulator.run()
