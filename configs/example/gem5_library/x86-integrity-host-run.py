# Copyright (c) 2024 CXL-NMP
# SPDX-License-Identifier: BSD-3-Clause
#
# Host-to-host DRAM bring-up for the ported IntegrityVerifier (Phase-2 step 3).
#
# A SINGLE X86 host CPU verifying HOST DRAM. No device, no CXL memory, no second
# System, no partitioning. The IntegrityVerifier is spliced between the shared
# L3 (LLC) and the memory bus by
# PrivateL1PrivateL2SharedL3IntegrityVerifierCacheHierarchy. Standard parallel
# fetch (no ECC). The engine is a traffic+latency model, so the bring-up gate is
# "does it instantiate, boot, and issue metadata traffic" -- NOT a crypto result.
#
# CPU modes (boot under a fast CPU, switch to the run CPU):
#   --boot-cpu atomic   (default; works WITHOUT KVM -- see note below)
#   --boot-cpu kvm      (faster boot, ONLY if this host's gem5/KVM is enabled)
#   --run-cpu  timing   (default; the MEANINGFUL run -- exercises modeled latency)
#   --run-cpu  atomic   (quick smoke; ATOMIC carries NO meaningful verifier
#                        latency, so it's a does-it-work gate only, not overhead)
#
# NOTE on KVM: an earlier build logged a "KVM not enabled" warning on this host.
# The default boot CPU is therefore ATOMIC so the bring-up is not blocked. If KVM
# is actually available, pass --boot-cpu kvm for a much faster boot.
#
# Usage (examples -- the USER runs these; this script does not build/run):
#   build/X86/gem5.opt configs/example/gem5_library/x86-integrity-host-run.py
#   build/X86/gem5.opt configs/example/gem5_library/x86-integrity-host-run.py \
#       --boot-cpu kvm --run-cpu timing --tree TimingBmt

import argparse

import m5
from m5.util.convert import toMemorySize

from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import SingleChannelDDR4_3200
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import DiskImageResource, KernelResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# The fresh, partition-free integrity-verifier hierarchy (step 3).
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_integrity_verifier_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3IntegrityVerifierCacheHierarchy,
)

# Known-good kernel + disk image for this repo (same as x86-cxl-f2-test.py).
KERNEL_PATH = "/home/alfi/CXL-NMP/fs_files/vmlinux_20240920"
DISK_PATH = "/home/alfi/CXL-NMP/fs_files/parsec.img"

_CPU = {
    "atomic": CPUTypes.ATOMIC,
    "timing": CPUTypes.TIMING,
    "kvm": CPUTypes.KVM,
}

parser = argparse.ArgumentParser(
    description="Host-to-host DRAM IntegrityVerifier bring-up."
)
parser.add_argument(
    "--boot-cpu",
    choices=["atomic", "kvm"],
    default="atomic",
    help="CPU used to boot Linux (default atomic; kvm only if enabled).",
)
parser.add_argument(
    "--run-cpu",
    choices=["atomic", "timing"],
    default="timing",
    help="CPU used after boot (default timing = meaningful run; atomic = quick "
    "smoke with no meaningful latency).",
)
parser.add_argument(
    "--tree",
    choices=["TimingBmt", "TimingTree"],
    default="TimingBmt",
    help="Integrity tree type (default TimingBmt).",
)
parser.add_argument("--arity", type=int, default=4, help="Integrity tree arity.")
parser.add_argument(
    "--mem",
    default="3GB",
    help="Total host DRAM (X86Board caps at 3GB). Default 3GB.",
)
parser.add_argument(
    "--reserve",
    default="1GiB",
    help="Bytes carved off the TOP of DRAM for the integrity structure. "
    "Default 1GiB (see sizing note below).",
)
args = parser.parse_args()

# ---------------------------------------------------------------------------
# DRAM range carve-out sizing.
#
# The verifier protects the OS-visible range and stores its integrity structure
# in (full - OS). For TimingBmt at arity 4 the structure is ~27% of the
# protected (OS) data: MACs ~25% (one 16B hash per 64B line) + counters ~1.6% +
# tree over counters (small). For a 2GiB OS region the BMT structure is ~553MB;
# for TimingTree it is ~715MB. A 1GiB reserve over a 3GB board leaves OS = 2GiB
# and 1GiB for integrity -- comfortably above both, so treeSizeValid() passes.
#
# The OS-visible cap is ENFORCED on Linux via `mem=<os>M`. This is REQUIRED, not
# cosmetic: if Linux touches the integrity range, a normal (non-metadata) packet
# lands in the integrity range and trips the verifier's
# assert(!rangeListContains(integrityRanges, addr)) in processReq().
# ---------------------------------------------------------------------------
full_bytes = toMemorySize(args.mem)
reserve_bytes = toMemorySize(args.reserve)
os_bytes = full_bytes - reserve_bytes
assert os_bytes > 0, "reserve must be smaller than total DRAM"
os_mib = os_bytes // (1024 * 1024)

boot_type = _CPU[args.boot_cpu]
run_type = _CPU[args.run_cpu]
need_switch = boot_type != run_type

requires(isa_required=ISA.X86, kvm_required=(args.boot_cpu == "kvm"))

# Single-channel DRAM => one contiguous [0, size) range (no interleave), which
# matches the verifier's "disjoint sorted ranges" assumption cleanly.
memory = SingleChannelDDR4_3200(size=args.mem)

cache_hierarchy = PrivateL1PrivateL2SharedL3IntegrityVerifierCacheHierarchy(
    l1d_size="32kB",
    l1d_assoc=8,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="256kB",
    l2_assoc=16,
    l3_size="2MB",
    l3_assoc=16,
    integrity_tree_type=args.tree,
    integrity_tree_arity=args.arity,
    integrity_allocation_mode="DramOnly",
    integrity_reserve_size=args.reserve,
    metadata_cache_size="128KiB",
    metadata_cache_assoc=8,
)

if need_switch:
    processor = SimpleSwitchableProcessor(
        starting_core_type=boot_type,
        switch_core_type=run_type,
        isa=ISA.X86,
        num_cores=1,
    )
else:
    # boot CPU == run CPU: no switch needed, single non-switchable processor.
    processor = SimpleProcessor(cpu_type=run_type, num_cores=1, isa=ISA.X86)

# This fork's X86Board ALWAYS attaches CXL plumbing (CXLBridge + CXLMemory PCI
# device + cxl_mem_bus + nmp_device) in _setup_io_devices(): it dereferences
# get_cxl_memory().get_size() UNCONDITIONALLY (x86_board.py:185-188), so
# cxl_memory is mandatory and cxl_memory=None would crash. There is no
# "no-CXL" host board in this fork. For this HOST-ONLY run the CXL device is
# attached but INERT: the workload is capped to host DRAM via `mem=<os>M`, so
# Linux never enrolls/touches the CXL E820 region at 0x100000000. We pass a
# small placeholder CXL memory purely to satisfy construction.
cxl_memory = SingleChannelDDR4_3200(size="512MB")

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_memory,  # mandatory in this fork; inert for host-only (see note)
    is_asic=True,  # only gates the inert CXL device's latency/size params; matches x86-cxl-run / f2 default
    # enable_nmp defaults to False -> no NMP membus->cxl_mem_bus bypass wiring.
)

# Enforce the carve-out: Linux uses only the OS-visible range; the top
# `reserve` bytes are left for the integrity structure (and must NOT be touched
# by the OS -- see the assert note above).
kernel_args = board.get_default_kernel_args() + [f"mem={os_mib}M"]

# Post-boot workload. The Linux boot itself drives the bulk of the host-DRAM
# traffic through the verifier; this adds a small run-CPU segment after the
# switch.
#
# NOTE: a guest `m5 resetstats` is deliberately NOT issued here. Under a KVM
# boot the simulation runs multi-threaded (a per-core event queue), and the
# guest `m5 resetstats` pseudo-instruction fires statistics::pythonReset(),
# which re-imports `m5.stats` into CPython WITHOUT holding the GIL. On the
# KVM-spawned worker thread that services the stats-reset barrier at the
# CPU switch, that off-GIL re-entry deterministically segfaults
# (PyImport_ImportModule -> PyUnicode_New). The stock KVM-switch example does
# not reset stats, which is why it never trips this.
#
# It is also redundant here: KVM boot bypasses the gem5 cache model, so the
# verifier accumulates ZERO traffic during the boot phase -- there is nothing
# to reset, and cumulative stats already approximate post-switch stats. If
# stats isolation is ever needed under a cache-traversing boot CPU, call
# m5.stats.reset() from a Python exit-event handler on the MAIN thread (GIL
# held), NOT via the guest pseudo-instruction on a worker thread.
_work = (
    "echo INTEGRITY_BRINGUP_RUNCPU;"
    "dd if=/dev/zero of=/dev/null bs=1M count=64;"
    "m5 exit;"
)
# The first `m5 exit` (only when switching) ends boot so we can switch CPUs.
command = ("m5 exit;" + _work) if need_switch else _work

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_PATH),
    readfile_contents=command,
    kernel_args=kernel_args,
)

on_exit = {}
if need_switch:
    # First EXIT (after boot): switch boot CPU -> run CPU and continue.
    # Second EXIT (after the workload): generator exhausted -> default exit.
    on_exit = {ExitEvent.EXIT: (func() for func in [processor.switch])}

simulator = Simulator(board=board, on_exit_event=on_exit)

print("=== IntegrityVerifier host-to-host DRAM bring-up ===")
print(f"  boot CPU      : {args.boot_cpu}    run CPU: {args.run_cpu}"
      f"    switch: {need_switch}")
print(f"  tree          : {args.tree}  arity {args.arity}  mode DramOnly")
print(f"  DRAM full     : [0, {full_bytes:#x})  ({full_bytes // (1<<20)} MiB)")
print(f"  DRAM OS       : [0, {os_bytes:#x})  ({os_bytes // (1<<20)} MiB)  "
      f"<- Linux capped via mem={os_mib}M")
print(f"  integrity     : [{os_bytes:#x}, {full_bytes:#x})  "
      f"({reserve_bytes // (1<<20)} MiB reserved)")
print("Running the simulation...")

simulator.run()
print(f"Exited @ tick {m5.curTick()} cause: {simulator.get_last_exit_event_cause()}")
