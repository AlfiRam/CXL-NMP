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
     silent. Concluded at the time: multiple KvmVMs in one gem5 process
     is unsupported.
     RESOLVED — that conclusion was wrong. The hang was an eventq_index
     collision: each processor's incorporate_processor numbers its KVM
     cores' event queues from 1 independently (base_cpu_processor.py:
     75-79), so the host core and device core 0 shared queue 1 — one
     thread, two vCPUs, and KVM's per-thread signal/timer setup
     (cpu/kvm/base.cc:254-261) starved one of them inside KVM_RUN.
     With globally disjoint queues (TwoSystemSimulator._instantiate)
     both Systems boot Linux under KVM concurrently (--dual-kvm, proven
     2026-07). Multi-KvmVM per process works: KvmVM is per-System with
     its own /dev/kvm+VM fds (vm.cc:316-324). --kvm-boot builds on
     this: KVM boot -> switch BOTH to ATOMIC at the boot barrier ->
     workload under ATOMIC.

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

Checkpoint/restore modes (added for fast iteration):

  --take-checkpoint DIR : normal cold boot of both Systems; the guests
     then PARK ALIVE in a minimal liveness loop (heartbeat + m5 exit +
     sleep — deliberately dumb: no readfile re-read, no cmp/cp, so this
     run validates the checkpoint MECHANISM only). When the serial
     barrier is met, m5.checkpoint(DIR) is taken at a quiescent point
     (NMP untouched, no DMA in flight) and the run exits. The exact
     argv is recorded in DIR/TAKE_CMDLINE.txt for precise replay.
  --restore DIR : rebuild the IDENTICAL config (same core counts,
     memory sizes, --is_asic) and restore from DIR instead of
     cold-booting. Success = both heartbeats appear in the FRESH
     outdir's serial files (the boot-time PASS strings live in the
     take-run's outdir and never reappear post-restore).

  HISTORY — the first take/restore attempt failed exactly where the
  original caveat predicted: CXLMemory::recvFunctional was an empty
  stub, so memWriteback() at checkpoint time silently dropped every
  dirty CXL-backed (NUMA node 1) cache line, and the restored host
  panicked on zeroed node-1 page tables. Fixed in cxl_memory.{hh,cc}
  (queue-sweeping functional forward, mirroring CXLBridge). The fix is
  now validated by this config: the device writes an ASCII sentinel
  into CXL DRAM during the take-run's parked state and every heartbeat
  re-reads it; the restore barrier requires the sentinel to read back
  correctly (device path), and host survival validates the host
  functional-write path (its node-1 page tables must round-trip).

  CONSEQUENCE of the dumb park loop: this checkpoint cannot have new
  work injected at restore (the parked script reads nothing beyond the
  sentinel address). The readfile-injection park loop comes in a LATER
  take-run — one more cold dual-boot — once this mechanism is proven.
"""

import argparse
import os
import sys
from pathlib import Path

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

        # DUAL-KVM FIX (proven by the --dual-kvm experiment): each
        # processor's incorporate_processor numbers its KVM cores' event
        # queues from 1 INDEPENDENTLY (base_cpu_processor.py:75-79; the
        # same per-processor numbering exists in switchable_processor.py:
        # 96-106, so SimpleSwitchableProcessor under --kvm-boot collides
        # identically), so with two boards the host core and device core 0
        # both land on queue 1 — one thread hosting two vCPUs from two
        # VMs. KVM's counter/timer signal delivery is set up per-thread
        # ("to ensure that signals are delivered to the right threads",
        # cpu/kvm/base.cc:254-261), so a shared queue can leave a vCPU
        # stuck in KVM_RUN with no timer kick — Attempt A's silent hang.
        # Renumber ALL KVM cores across BOTH boards into one global 1..N
        # range (queue 0 stays reserved for every non-CPU SimObject, which
        # incorporate_processor already assigned; a SwitchableProcessor's
        # get_cores() returns its CURRENT cores — the KVM start cores —
        # and its switched-out ATOMIC cores stay on queue 0). This must
        # happen after _pre_instantiate (which assigns the colliding
        # indices) and before m5.instantiate (which sizes the event-queue
        # array).
        kvm_cores = [
            c
            for proc in (self._board.processor, self._device_board.processor)
            for c in proc.get_cores()
            if c.is_kvm_core()
        ]
        for i, core in enumerate(kvm_cores):
            core.get_simobject().eventq_index = i + 1
        if kvm_cores:
            queues = [c.get_simobject().eventq_index for c in kvm_cores]
            assert len(set(queues)) == len(queues), (
                "KVM event-queue renumbering failed to produce disjoint "
                f"queues: {queues}"
            )
            print(
                f"[kvm-eventq] {len(kvm_cores)} KVM cores on disjoint event "
                f"queues {queues} (queue 0 = all other SimObjects)"
            )

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
        dev_proc = self._device_board.processor
        any_kvm = (
            any(c.is_kvm_core() for c in host_proc.get_cores())
            or any(c.is_kvm_core() for c in dev_proc.get_cores())
            or (
                isinstance(host_proc, SwitchableProcessor)
                and any(c.is_kvm_core() for c in host_proc._all_cores())
            )
            or (
                isinstance(dev_proc, SwitchableProcessor)
                and any(c.is_kvm_core() for c in dev_proc._all_cores())
            )
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
    checkpoint_dir: str = None,
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

    If `checkpoint_dir` is set, m5.checkpoint(checkpoint_dir) is called
    at the moment the barrier is met, BEFORE yielding True. At that
    point both guests are parked in their liveness loops: NMP device
    untouched, no DMA in flight — the quiescent point required because
    none of the custom CXL objects (cxl_bridge.cc, cxl_memory.cc,
    cxl_nmp_device.cc) implements serialize() or drain().

    HONEST CAVEAT — a clean checkpoint here is EVIDENCE, NOT PROOF that
    the CXL path survives save/restore. m5.checkpoint() runs
    memWriteback() first, and the host kernel enrolls CXL DRAM as
    NUMA-node-1 RAM, so dirty CXL-backed cache lines (if any exist at
    boot) must flush FUNCTIONALLY through CXLBridge -> CXLMemory.
    CXLBridge supports functional access (cxl_bridge.cc:477); CXLMemory
    has no functional-path code of its own. And the restored run
    deliberately never reads CXL memory back. CXL-path integrity across
    checkpoint/restore remains UNVERIFIED until a later run has a
    restored guest read back known CXL contents.
    """
    while True:
        host_done = _serial_contains(host_serial_path, host_pass)
        dev_done = _serial_contains(device_serial_path, device_pass)
        if host_done and dev_done:
            if checkpoint_dir is not None:
                print(
                    f"[barrier] both PASS strings seen — "
                    f"writing checkpoint to {checkpoint_dir}"
                )
                # Same pattern as stdlib save_checkpoint_generator:
                # m5.checkpoint() drains, runs memWriteback over both
                # Systems, then serializes the global simObjectList.
                m5.checkpoint(checkpoint_dir)
                # Record the exact invocation so the restore run can
                # replay an identical topology (restore matches
                # checkpoint sections to SimObject paths; core count,
                # memory sizes and --is_asic must not drift).
                with open(
                    os.path.join(checkpoint_dir, "TAKE_CMDLINE.txt"), "w"
                ) as f:
                    f.write("taken by: " + " ".join(sys.argv) + "\n")
                    # Machine-readable record; the restore-time check
                    # parses this line and rejects a differing
                    # --device-cores rather than failing at opaque
                    # section-mismatch time.
                    f.write(f"device_cores: {args.device_cores}\n")
                print("[barrier] checkpoint written — ending simulation")
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


def kvm_boot_barrier_handler(
    host_serial_path: str,
    device_serial_path: str,
    boot_host: str,
    boot_dev: str,
    done_host: str,
    done_dev: str,
    host_processor,
    device_processor,
    host_readfile_path: str,
    device_readfile_path: str,
    host_phase2: str,
    device_phase2: str,
):
    """Two-phase barrier for --kvm-boot.

    Phase 1 (KVM boot): every exit event rechecks the serials for BOTH
    boot PASS strings. The guests park in a heartbeat/poll loop after
    their PASS echo, so exit events keep arriving — this is what makes
    the barrier robust to the KVM serial-drain race (a PASS string can
    land in the file AFTER the exit event of the m5op that followed it;
    diagnosed on the --dual-kvm smoke run, where the run's LAST event
    preceded the device's PASS landing and the barrier starved).

    At the barrier: BOTH processors switch KVM->ATOMIC together, never
    per-System — a lone ATOMIC System coexisting with a still-booting
    KVM System recreates Attempt B's TSC-drift hazard (module
    docstring). m5.switchCpus does the takeover and flips each System's
    memory mode itself (m5/simulate.py, "Change the memory mode if
    required"). The workload is then RELEASED by rewriting each board's
    readfile with the phase-2 script: pseudo_inst::readfile re-opens
    the file on every call (src/sim/pseudo_inst.cc:376-384), so the
    guests' poll loops observe the marker on their next iteration and
    exec the workload — entirely under ATOMIC.

    Phase 2 (ATOMIC workload): terminate when both completion strings
    appear. The phase-2 scripts end in the same heartbeat loop, so the
    completion strings are also drain-race-immune.
    """
    switched = False
    while True:
        if not switched:
            hb = _serial_contains(host_serial_path, boot_host)
            db = _serial_contains(device_serial_path, boot_dev)
            if hb and db:
                print(
                    "[kvm-boot] boot barrier met — switching BOTH "
                    "processors KVM->ATOMIC together"
                )
                with open(host_readfile_path, "w") as f:
                    f.write(host_phase2)
                with open(device_readfile_path, "w") as f:
                    f.write(device_phase2)
                host_processor.switch()
                device_processor.switch()
                switched = True
                print(
                    "[kvm-boot] switch complete; phase-2 workload "
                    "injected via readfile — continuing under ATOMIC"
                )
                yield False
            else:
                print(
                    f"[kvm-boot] boot barrier not met "
                    f"(host={hb}, dev={db}) — continuing under KVM"
                )
                yield False
        else:
            hd = _serial_contains(host_serial_path, done_host)
            dd = _serial_contains(device_serial_path, done_dev)
            if hd and dd:
                print(
                    "[kvm-boot] workload barrier met "
                    f"(host_done={hd}, dev_done={dd}) — ending simulation"
                )
                yield True
            else:
                print(
                    f"[kvm-boot] post-switch; workload barrier not met "
                    f"(host={hd}, dev={dd}) — continuing"
                )
                yield False


# =============================================================================
# CLI
# =============================================================================
requires(isa_required=ISA.X86)

parser = argparse.ArgumentParser(
    description="Two-System x86 Linux smoke test."
)
parser.add_argument(
    "--is_asic",
    type=str,
    nargs="?",
    choices=["True", "False"],
    default="True",
    help="ASIC vs FPGA CXL device latency model (host-side only).",
)
parser.add_argument(
    "--take-checkpoint",
    metavar="DIR",
    default=None,
    help="Cold-boot both Systems; when the serial barrier is met "
    "(both guests booted and parked in their liveness loops), "
    "checkpoint into DIR and exit cleanly.",
)
parser.add_argument(
    "--restore",
    metavar="DIR",
    default=None,
    help="Restore both Systems from the checkpoint in DIR instead of "
    "cold-booting. The config must be IDENTICAL to the "
    "--take-checkpoint run (same core counts, memory sizes, "
    "--is_asic) — see TAKE_CMDLINE.txt inside DIR.",
)
parser.add_argument(
    "--atomic",
    action="store_true",
    help="Boot both Systems under ATOMIC CPUs instead of TIMING. "
    "Dev-speed variant for functional work (the host->device handshake): "
    "iterates in minutes, NO timing fidelity. Use TIMING (the default) "
    "for timing-accurate/benchmark runs.",
)
parser.add_argument(
    "--dual-kvm",
    action="store_true",
    help="EXPERIMENT: boot BOTH Systems under KVM cores. Exists purely to "
    "answer one question — does dual-KVM boot when the two Systems' KVM "
    "vCPUs are on DISJOINT event queues? (Attempt A in the module docstring "
    "hung with both serials silent; the suspected cause is the stdlib's "
    "per-processor eventq numbering, base_cpu_processor.py:75-79, which "
    "lands the host core and device core 0 on the SAME queue — KVM's "
    "signal/timer setup is thread-specific, base.cc:254-261. "
    "TwoSystemSimulator._instantiate renumbers the queues disjointly.) "
    "Boot-only smoke test: not combinable with any other mode flag.",
)
parser.add_argument(
    "--kvm-boot",
    action="store_true",
    help="Boot BOTH Systems under KVM (fast, minutes), then switch BOTH to "
    "ATOMIC at the boot barrier and run the workload under ATOMIC. Uses "
    "SimpleSwitchableProcessor(KVM->ATOMIC) per board plus the disjoint "
    "event-queue renumbering proven by --dual-kvm. The guests park in a "
    "heartbeat/poll loop after boot; when both PASS strings are in serial "
    "the handler switches both processors TOGETHER (never per-System — "
    "Attempt B's TSC hazard) and releases the workload by rewriting each "
    "board's readfile (m5 readfile re-reads the file per call, "
    "pseudo_inst.cc:376). Composes with --offload/--device-integrity/"
    "--device-handoff; the workload runs entirely under ATOMIC.",
)
parser.add_argument(
    "--kvm-no-perf",
    action="store_true",
    help="With --dual-kvm/--kvm-boot: set usePerf=False on every KVM core. "
    "Diagnostic fallback ONLY — use if the KVM run dies at startup with a "
    "perf_event_open error (host perf_event_paranoid too strict) so a perf "
    "permission failure is not misread as the Attempt-A hang.",
)
parser.add_argument(
    "--device-cores",
    type=int,
    default=2,
    metavar="N",
    help="Number of device cores (default: 2, the historical hardcoded "
    "value). Used by BOTH device-processor branches (KVM and non-KVM). "
    "Use 1 under --kvm-boot/--dual-kvm to sidestep the wait-for-SIPI AP "
    "unpark issue (a parked KVM AP has no re-park path once a stray "
    "interrupt wakes it; see the comment at the device-processor "
    "construction). Checkpoints record this value; --restore rejects a "
    "mismatch.",
)
parser.add_argument(
    "--offload",
    action="store_true",
    help="MVP host->device code offload over a shared CXL mailbox. Host "
    "carves the top 16 MiB of CXL as Reserved (memmap=) and runs "
    "/home/cxl_benchmark/host_offload; device runs device_offload. "
    "Intended with --atomic. Requires both binaries installed in the "
    "guest image (benchmarks/Makefile `install`).",
)
parser.add_argument(
    "--device-integrity",
    action="store_true",
    help="Splice an IntegrityVerifier on the DEVICE System's read-from-CXL "
    "(ingress) path, between the device membus and device_iobridge. The "
    "verifier protects the shared CXL window (CxlOnly, TimingBmt) and is "
    "inert/pass-through for the offload mailbox region. Default off. Under "
    "--atomic the gate is build+boot+routing (config.ini), not stats.",
)
parser.add_argument(
    "--device-handoff",
    action="store_true",
    help="§6.2 range-keyed subtree handoff (requires --device-integrity). The "
    "host hands the device AUTHORITY over a contiguous region at the start of "
    "the device's protected CXL window; the device verifier re-roots its "
    "integrity walk for that range. The host offload runs the OP_HANDOFF "
    "dispatch: XTEA blob ships in the mailbox (control channel) but the "
    "key/plaintext operands are written INTO the handed-off protected region "
    "(data path); the device prints the handoff receipt, reads the operands "
    "out of the region, executes the blob, and returns the ciphertext. A "
    "second host memmap= carve makes the region /dev/mem-mmappable. Default "
    "off. Under --atomic the gate is config + receipt + ciphertext match "
    "(functional data-binding), not stats.",
)
args = parser.parse_args()
if args.take_checkpoint and args.restore:
    parser.error("--take-checkpoint and --restore are mutually exclusive.")
if args.offload and (args.take_checkpoint or args.restore):
    parser.error(
        "--offload cannot be combined with --take-checkpoint/--restore."
    )
if args.device_handoff and not args.device_integrity:
    parser.error("--device-handoff requires --device-integrity.")
if args.device_handoff and not args.offload:
    parser.error("--device-handoff requires --offload (it rides the mailbox).")
# A checkpoint encodes its CPU class + mem_mode; ATOMIC and TIMING cannot
# cross-restore. Fail fast rather than at opaque section-mismatch time.
if args.atomic and (args.take_checkpoint or args.restore):
    parser.error(
        "--atomic cannot be combined with --take-checkpoint/--restore: "
        "a checkpoint's CPU class and mem_mode are mode-specific."
    )
# --dual-kvm is a boot-only experiment (does dual-KVM boot with disjoint
# event queues?). Reject every other mode flag so the result answers
# exactly that question, with no offload/verifier/checkpoint variables.
if args.dual_kvm and (
    args.atomic
    or args.offload
    or args.take_checkpoint
    or args.restore
    or args.device_integrity
    or args.device_handoff
):
    parser.error(
        "--dual-kvm is a boot-only smoke test; it cannot be combined with "
        "--atomic/--offload/--take-checkpoint/--restore/--device-integrity/"
        "--device-handoff."
    )
if args.kvm_boot and args.dual_kvm:
    parser.error(
        "--kvm-boot and --dual-kvm are mutually exclusive (--dual-kvm is "
        "the boot-only experiment; --kvm-boot is the production KVM->ATOMIC "
        "flow)."
    )
if args.kvm_boot and args.atomic:
    parser.error(
        "--kvm-boot already ends in ATOMIC (KVM boot, ATOMIC workload); "
        "drop --atomic."
    )
if args.kvm_boot and (args.take_checkpoint or args.restore):
    parser.error(
        "--kvm-boot cannot be combined with --take-checkpoint/--restore "
        "yet: switched-out cores and the mid-run readfile rewrite are not "
        "part of the proven checkpoint mechanism."
    )
if args.kvm_no_perf and not (args.dual_kvm or args.kvm_boot):
    parser.error("--kvm-no-perf only applies to --dual-kvm/--kvm-boot.")
if args.device_cores < 1:
    parser.error("--device-cores must be >= 1.")
# A checkpoint serializes per-core state (CPU threads, per-core lapic
# pendingInit/startedUp — interrupts.cc:774-801), so restoring with a
# different device core count fails at opaque section-mismatch time.
# Fail fast instead, using the count recorded in TAKE_CMDLINE.txt.
# Checkpoints predating --device-cores carry no record; they were all
# taken with the then-hardcoded 2.
if args.restore:
    _taken_cores = 2
    try:
        with open(os.path.join(args.restore, "TAKE_CMDLINE.txt")) as _f:
            for _line in _f:
                if _line.startswith("device_cores:"):
                    _taken_cores = int(_line.split(":", 1)[1])
    except FileNotFoundError:
        pass
    if _taken_cores != args.device_cores:
        parser.error(
            f"checkpoint {args.restore} was taken with device_cores="
            f"{_taken_cores}, but this run requests --device-cores "
            f"{args.device_cores}. Per-core state is serialized per core, "
            f"so the topologies must match exactly; re-run with "
            f"--device-cores {_taken_cores}."
        )

# Dev-speed vs timing-accurate selector. ATOMIC flips BOTH Systems'
# SimpleProcessors; each board's incorporate_processor then sets its own
# System.mem_mode to 'atomic' automatically (base_cpu_processor.py:99).
# Classic caches are retained in either mode ('atomic', not the KVM/Ruby
# 'atomic_noncaching' downgrade). ATOMIC + the custom CXL objects is
# already proven by x86-cxl-run.py's atomic-boot path.
# --dual-kvm flips both to KVM cores instead (mem_mode becomes
# 'atomic_noncaching' via the same incorporate_processor path; caches are
# present but bypassed — irrelevant for the boot-only experiment).
_cpu_type = (
    CPUTypes.KVM
    if args.dual_kvm
    else CPUTypes.ATOMIC
    if args.atomic
    else CPUTypes.TIMING
)


# =============================================================================
# Host X86Board — same configuration as x86-cxl-ptr-chase-test.py so
# the host-side regression test still passes against the same gem5 binary.
# =============================================================================
host_cache = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)

host_memory = DIMM_DDR5_4400(size="3GB")
if args.is_asic == "True":
    host_cxl_memory = DIMM_DDR5_4400(size="8GB")
else:
    host_cxl_memory = SingleChannelDDR4_3200(size="8GB")

# Host CPU: TIMING by default, ATOMIC under --atomic, KVM under --dual-kvm.
# Under --kvm-boot: a switchable KVM->ATOMIC pair. The KVM start cores and
# the ATOMIC switch cores share SimObject paths, and m5.switchCpus performs
# the takeover + per-System memory-mode change itself (m5/simulate.py,
# "Change the memory mode if required").
if args.kvm_boot:
    host_processor = SimpleSwitchableProcessor(
        starting_core_type=CPUTypes.KVM,
        switch_core_type=CPUTypes.ATOMIC,
        isa=ISA.X86,
        num_cores=1,
    )
else:
    host_processor = SimpleProcessor(
        cpu_type=_cpu_type,
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
    l1d_size="32kB",
    l1i_size="32kB",
    l2_size="512kB",
)
device_memory = SingleChannelDDR3_1600(size="512MB")

# Device: TIMING-only from tick 0 (dual-KVM fallback documented in the
# module docstring). No switchable processor, no usePerf hack — those
# are KVM-specific concerns.
#
# DEVICE CORE COUNT — --device-cores N (default 2, the historical
# hardcoded value); one variable feeds BOTH branches so the KVM and
# non-KVM paths can never drift. The static plumbing scales correctly
# via get_num_cores(): the IntelMP table and apicbridge range in
# device_x86_board.py, and per-core L1/L2 in the cache hierarchy
# (proven through SMP bring-up by the 2026-07 dual-KVM runs — the old
# claim that multi-core KVM is foreclosed in two-System runs is
# obsolete). The DYNAMIC caveat is under KVM: a wait-for-SIPI AP is
# parked by tc->suspend() with nothing pending (fs_workload.cc:112-120),
# but Interrupts::requestInterrupt unconditionally calls cpu->wakeup()
# (interrupts.cc:271-273) and BaseKvmCPU::wakeup() activates any
# suspended thread — and unlike the simulated CPUs' INIT-microcode halt
# loop, a KVM core has no re-park path. A stray pre-SIPI interrupt
# therefore leaves the AP executing reset-vector garbage until the real
# INIT/SIPI (diagnosed 2026-07 on the nested-KVM machine). Until that
# is fixed, use --device-cores 1 for KVM boots that hit SMP bring-up
# hangs.
if args.kvm_boot:
    device_processor = SimpleSwitchableProcessor(
        starting_core_type=CPUTypes.KVM,
        switch_core_type=CPUTypes.ATOMIC,
        isa=ISA.X86,
        num_cores=args.device_cores,
    )
else:
    device_processor = SimpleProcessor(
        cpu_type=_cpu_type,  # ATOMIC under --atomic; see _cpu_type above
        isa=ISA.X86,
        num_cores=args.device_cores,
    )

if args.kvm_no_perf:
    # Diagnostic fallback (see --kvm-no-perf help): drop the per-vCPU perf
    # counters so a perf_event_open permission failure can't masquerade as
    # the Attempt-A hang. usePerf defaults True (BaseKvmCPU.py:66); with it
    # False the KVM CPU paces itself by the host timer alone (the
    # usePerfOverflow default is False, so base.cc:103's fatal cannot fire).
    # get_cores() returns the CURRENT cores — under --kvm-boot those are the
    # KVM start cores (the ATOMIC switch cores have no usePerf param, so the
    # is_kvm_core guard matters there).
    for _core in (*host_processor.get_cores(), *device_processor.get_cores()):
        if _core.is_kvm_core():
            _core.get_simobject().usePerf = False

device_board = DeviceX86Board(
    clk_freq="1GHz",
    processor=device_processor,
    memory=device_memory,
    cache_hierarchy=device_cache,
    cxl_mem_bus=cxl_mem_bus,
    cxl_mem_range=cxl_mem_range,
    # Phase 3 M1: optional device-side verifier on the CXL-ingress path.
    # Default off -> existing offload path unchanged. Ranges default inside
    # the board (CxlOnly: protect bottom 2 GiB, exclude top-16 MiB mailbox).
    cxl_integrity=args.device_integrity,
    # Phase 3 M2: optional range-keyed subtree handoff (the board computes the
    # handed-off region at the start of cxl_os and configures the verifier).
    cxl_handoff=args.device_handoff,
)


# =============================================================================
# Workloads — distinct readfiles per System so each board's `m5 readfile`
# in the guest reads its own command stream.
# =============================================================================
KERNEL_PATH = "/home/alfi/CXL-NMP/fs_files/vmlinux_20240920"
DISK_PATH = "/home/alfi/CXL-NMP/fs_files/parsec.img"

# Default (smoke-test) mode: one-shot PASS + m5 exit, unchanged.
# Both Systems are TIMING from tick 0 — no leading `m5 exit;` (no switch).
host_cmd = (
    "echo '=== host: X86Board on TIMING ===';"
    + "echo 'host boot OK';"
    + "m5 exit;"
)
device_cmd = (
    "echo '=== device: DeviceX86Board on TIMING ===';"
    + "echo 'device boot OK';"
    + "m5 exit;"
)

# Checkpoint mode: print PASS, then PARK ALIVE in a minimal liveness
# loop. Attempt C (module docstring) showed that after a one-shot
# script ends, guest userspace dies and the last thread context
# suspends — a checkpoint of that state restores a dead guest.
#
# DELIBERATELY DUMB: just heartbeat + m5 exit + sleep. No readfile
# re-read, no cmp/cp. This run validates the checkpoint MECHANISM only
# (first 2-core SMP-TIMING boot, first two-System checkpoint, first
# save/restore over non-serialization-aware CXL objects); a smarter
# readfile-injection loop would entangle a fourth unproven mechanism
# with the restore path. CONSEQUENCE: this checkpoint cannot have new
# work injected at restore — the parked script is baked into guest
# memory and reads nothing. The workload-injection park loop comes in
# a LATER take-run (one more cold boot), once this mechanism is proven.
#
# The heartbeat doubles as the restore marker: a restored run starts
# in a fresh outdir with empty serial files, so the barrier must key
# on output generated POST-restore — which a periodic heartbeat is,
# within ~2 simulated seconds of resume.
_liveness_loop = (
    "while true; do " "echo 'host alive'; " "m5 exit; sleep 2; " "done"
)

# CXL sentinel (device side). The DEVICE sees CXL as a Reserved E820
# region, so /dev/mem access is permitted under STRICT_DEVMEM — the
# same mechanism the proven device_cxl_test binary uses (mmap there,
# dd here). Offset 64 KiB into the CXL range, matching that test's
# TEST_OFFSET. 0x100010000 = 4295032832.
#
# The device writes the sentinel ONCE post-boot, and every heartbeat
# re-reads it from CXL and prints it. Post-restore, device caches are
# cold (never serialized), so the read must traverse device_iobridge
# -> cxl_mem_bus -> DRAM: a true restored-content readback. The HOST
# path (CXLMemory::recvFunctional fix) is validated by host survival —
# the host kernel's own node-1 page tables are the sentinel there.
#
# ACCEPTED RISK: the host kernel owns this address as NUMA-node-1 RAM;
# 17 bytes here could clobber host data if that page is allocated.
# Boot-time node-1 allocations were observed near the TOP of the range
# (the restore-bug crash dump), so bottom+64KiB is very likely free,
# but unguaranteed until a proper shared-scratch carve-out exists.
# There is also no coherence between host caches and device-side CXL
# writes — harmless while the host is parked idle.
_CXL_SENTINEL = "CXLSENTINEL_F2_OK"  # 17 bytes, ASCII so serial shows it
_CXL_SENTINEL_ADDR = 4295032832  # 0x100010000
_write_sentinel = (
    f"printf '%s' '{_CXL_SENTINEL}' | "
    f"dd of=/dev/mem bs=1 seek={_CXL_SENTINEL_ADDR} conv=notrunc 2>/dev/null; "
)
_read_sentinel = (
    f"$(dd if=/dev/mem bs=1 skip={_CXL_SENTINEL_ADDR} "
    f"count={len(_CXL_SENTINEL)} 2>/dev/null)"
)
_device_loop = (
    "while true; do "
    f'echo "device alive CXL={_read_sentinel}"; '
    "m5 exit; sleep 2; "
    "done"
)
if args.take_checkpoint or args.restore:
    host_cmd = "echo 'host boot OK'; " + _liveness_loop
    device_cmd = "echo 'device boot OK'; " + _write_sentinel + _device_loop
    # In restore mode these contents are never read by the guests (the
    # parked script reads nothing); they are written only so the
    # config-time file plumbing is identical to the take-run.

# Offload mode (MVP host->device CXL dispatch). Each side runs its offload
# binary from the guest image. The programs rendezvous through the shared
# CXL mailbox (host arms + rings the doorbell, device polls + executes +
# returns a result); they self-synchronize, so host/device boot-order skew
# is harmless. Every exit path of each binary prints its
# "HOST OFFLOAD"/"DEVICE OFFLOAD" prefix, so the barrier (below) ends the
# run on failure (TIMEOUT/ERROR) as well as success. The host's memmap=
# carve-out is added to its kernel_args (below). Intended with --atomic.
if args.offload:
    if args.device_handoff:
        # The board computed the handed-off region (start of cxl_os). Thread its
        # base+size into the host guest so host_offload writes the OP_HANDOFF
        # descriptor AND places the XTEA operands inside the region (at
        # base+HANDOFF_OPS_OFF); the device prints the receipt, reads the
        # operands out of the protected region, and runs the blob. Verifier was
        # already configured with the same range at instantiation.
        _ho_base = device_board._cxl_handoff_start
        _ho_size = device_board._cxl_handoff_size
        _host_work = f"/home/cxl_benchmark/host_offload handoff {_ho_base:#x} {_ho_size};"
    else:
        _host_work = "/home/cxl_benchmark/host_offload;"
    _device_work = "/home/cxl_benchmark/device_offload;"
    host_cmd = "echo 'host boot OK'; " + _host_work + " m5 exit;"
    device_cmd = "echo 'device boot OK'; " + _device_work + " m5 exit;"

# --kvm-boot: two-phase guest scripts. Phase 1 (the initial readfile) prints
# the boot PASS and PARKS in a heartbeat/poll loop; phase 2 (the workload) is
# injected by the handler at switch time by REWRITING the readfile —
# pseudo_inst::readfile re-opens the file per call (pseudo_inst.cc:376-384).
#
# Why the park loop (and not a one-shot `sleep 2; m5 exit`): (a) the
# early-booting System must wait an UNBOUNDED time for the late one, so no
# fixed number of extra exits suffices; (b) the loop's periodic m5 exits are
# what make the boot barrier immune to the KVM serial-drain race (the PASS
# string can land in the file after the exit event that followed it — the
# diagnosed --dual-kvm hang); (c) polling `m5 readfile` doubles as the
# release mechanism, so the workload provably starts only after the switch.
# This is the same parked-heartbeat pattern the checkpoint path validated.
#
# The grep pattern is quote-split so the phase-1 script does NOT itself
# contain the contiguous marker (else the poll would match its own script
# text and re-exec phase 1 as the workload).
_KVM_PHASE2_MARKER = "KVMBOOT_PHASE2_GO"
_kvm_marker_grep = 'KVMBOOT_"PHASE2"_GO'


def _kvm_phase1(pass_echo: str) -> str:
    return (
        f"echo '{pass_echo}'; "
        "while true; do "
        "m5 exit; "
        f"if m5 readfile | grep -q {_kvm_marker_grep}; then "
        "m5 readfile > /tmp/phase2.sh; sh /tmp/phase2.sh; break; "
        "fi; "
        "sleep 1; "
        "done"
    )


# Post-workload heartbeat: keeps exit events coming so the completion
# strings are also drain-race-immune (the barrier just needs one event
# after the strings land).
_KVM_PHASE2_TAIL = "while true; do m5 exit; sleep 2; done\n"

if args.kvm_boot:
    if args.offload:
        _host_phase2 = (
            f"# {_KVM_PHASE2_MARKER}\n"
            "echo 'host phase2 start (ATOMIC)'\n"
            f"{_host_work}\n" + _KVM_PHASE2_TAIL
        )
        _device_phase2 = (
            f"# {_KVM_PHASE2_MARKER}\n"
            "echo 'device phase2 start (ATOMIC)'\n"
            f"{_device_work}\n" + _KVM_PHASE2_TAIL
        )
    else:
        # Plain --kvm-boot smoke: phase 2 just proves the switch landed and
        # the guests still execute post-switch.
        _host_phase2 = (
            f"# {_KVM_PHASE2_MARKER}\n"
            "echo 'host post-switch OK'\n" + _KVM_PHASE2_TAIL
        )
        _device_phase2 = (
            f"# {_KVM_PHASE2_MARKER}\n"
            "echo 'device post-switch OK'\n" + _KVM_PHASE2_TAIL
        )
    host_cmd = _kvm_phase1("host boot OK")
    device_cmd = _kvm_phase1("device boot OK")

host_workload_kwargs = dict(
    kernel=KernelResource(local_path=KERNEL_PATH),
    disk_image=DiskImageResource(local_path=DISK_PATH),
    readfile=os.path.join(m5.options.outdir, "host_readfile"),
    readfile_contents=host_cmd,
)
if args.offload:
    # Carve the top 16 MiB of the CXL window as Reserved on the HOST so
    # host_offload can mmap /dev/mem at the mailbox base (0x2ff000000) —
    # the host otherwise enrolls all of CXL as type-1 RAM, which
    # STRICT_DEVMEM blocks from /dev/mem. memmap= also keeps the host page
    # allocator out of it (no clobber). The device board is untouched: it
    # already sees the whole CXL window as Reserved. Reuses the board's own
    # default kernel args + the carve-out. Must match MB_BASE/MB_SIZE in
    # benchmarks/cxl_mailbox.h.
    _host_memmap_args = ["memmap=16M$0x2ff000000"]
    if args.device_handoff:
        # Second carve: the handed-off protected region at the BOTTOM of the
        # CXL window. The host enrolls the window as type-1 RAM, so without
        # this the region is host page-allocator memory and STRICT_DEVMEM
        # blocks /dev/mem there — host_offload could neither reach it nor
        # safely write operands into it. Reserved => mmappable + host kernel
        # keeps out (and the mapping is UC, atomic-safe, like the mailbox).
        _ho_base = device_board._cxl_handoff_start
        _ho_size = device_board._cxl_handoff_size
        assert (
            _ho_size % (1 << 20) == 0
        ), "handoff region size must be MiB-aligned for the memmap= carve"
        _host_memmap_args.append(f"memmap={_ho_size >> 20}M${_ho_base:#x}")
    host_workload_kwargs["kernel_args"] = (
        host_board.get_default_kernel_args() + _host_memmap_args
    )
if args.restore:
    # TwoSystemSimulator._instantiate consults ONLY the host board's
    # _checkpoint (its existing `if self._board._checkpoint:` branch).
    # m5.instantiate(dir) restores ALL SimObjects globally — including
    # everything under Root.systems — so the device board needs no
    # checkpoint reference of its own.
    host_workload_kwargs["checkpoint"] = Path(args.restore)
host_board.set_kernel_disk_workload(**host_workload_kwargs)
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
HOST_SERIAL = os.path.join(m5.options.outdir, "board.pc.com_1.device")
DEVICE_SERIAL = os.path.join(m5.options.outdir, "systems.pc.com_1.device")

# Restored runs key on the heartbeats (generated post-restore in the
# fresh outdir), NOT the boot-time PASS strings (printed pre-checkpoint
# into the take-run's outdir).
if args.restore:
    # Sentinel readback IS the restore success criterion (device path);
    # host survival + heartbeat validates the host functional-write path.
    PASS_HOST = "host alive"
    PASS_DEV = f"CXL={_CXL_SENTINEL}"
elif args.take_checkpoint:
    # Require a sentinel heartbeat BEFORE checkpointing, so a broken
    # sentinel write/read can never produce a poisoned checkpoint.
    PASS_HOST = "host boot OK"
    PASS_DEV = f"CXL={_CXL_SENTINEL}"
elif args.offload:
    # Both offload programs print their prefix on EVERY exit path
    # (OK/FAIL/TIMEOUT/ERROR), so the barrier ends the run on failure as
    # well as success — operator reads the actual status in serial.
    # (Also the phase-2 termination strings under --kvm-boot --offload.)
    PASS_HOST = "HOST OFFLOAD"
    PASS_DEV = "DEVICE OFFLOAD"
elif args.kvm_boot:
    # Plain --kvm-boot smoke: terminate on the phase-2 post-switch echoes
    # (the boot PASS strings only drive the SWITCH barrier in this mode).
    PASS_HOST, PASS_DEV = "host post-switch OK", "device post-switch OK"
else:
    PASS_HOST, PASS_DEV = "host boot OK", "device boot OK"

if args.kvm_boot:
    # Two-phase handler: boot barrier (switch BOTH + inject phase 2) then
    # workload barrier (terminate on PASS_HOST/PASS_DEV).
    _exit_handler = kvm_boot_barrier_handler(
        host_serial_path=HOST_SERIAL,
        device_serial_path=DEVICE_SERIAL,
        boot_host="host boot OK",
        boot_dev="device boot OK",
        done_host=PASS_HOST,
        done_dev=PASS_DEV,
        host_processor=host_processor,
        device_processor=device_processor,
        host_readfile_path=os.path.join(m5.options.outdir, "host_readfile"),
        device_readfile_path=os.path.join(
            m5.options.outdir, "device_readfile"
        ),
        host_phase2=_host_phase2,
        device_phase2=_device_phase2,
    )
else:
    _exit_handler = serial_barrier_handler(
        host_serial_path=HOST_SERIAL,
        device_serial_path=DEVICE_SERIAL,
        host_pass=PASS_HOST,
        device_pass=PASS_DEV,
        checkpoint_dir=args.take_checkpoint,
    )

simulator = TwoSystemSimulator(
    board=host_board,
    device_board=device_board,
    on_exit_event={
        ExitEvent.EXIT: _exit_handler,
    },
)

_cpu_label = (
    "KVM->ATOMIC"
    if args.kvm_boot
    else "KVM"
    if args.dual_kvm
    else "ATOMIC"
    if args.atomic
    else "TIMING"
)
print("=" * 80)
print(f"Two-System smoke test: {_cpu_label}-only + serial-output barrier")
print("=" * 80)
print(
    f"  CPU mode       : {'ATOMIC (dev-speed, no timing fidelity)' if args.atomic else 'TIMING'}"
)
print(
    f"  Host board     : X86Board  ({_cpu_label}-only)  with CXLNMPDevice etc."
)
print(
    f"  Device board   : DeviceX86Board  ({_cpu_label}-only)  + device_iobridge"
)
print(f"  cxl_mem_bus    : host_board.cxl_mem_bus  (CXLMemBar, 3 masters)")
print(
    f"  CXL range      : 0x100000000, size {host_board.get_cxl_memory().get_size_str()}"
)
print(f"  Host readfile  : {os.path.join(m5.options.outdir, 'host_readfile')}")
print(
    f"  Dev  readfile  : {os.path.join(m5.options.outdir, 'device_readfile')}"
)
print(f"  Host serial    : {HOST_SERIAL}")
print(f"  Dev  serial    : {DEVICE_SERIAL}")
print(f"  Barrier        : sim ends when BOTH PASS strings appear in serial")
print(f"                   ('{PASS_HOST}' AND '{PASS_DEV}')")
print(
    f"  Wall-clock     : {'~minutes cold boot (ATOMIC dev-speed)' if args.atomic else 'both ~25-30 min cold boot (parallel under TIMING)'}"
)
mode = (
    "take-checkpoint"
    if args.take_checkpoint
    else "restore"
    if args.restore
    else "offload"
    if args.offload
    else "dual-kvm boot experiment"
    if args.dual_kvm
    else "smoke-test"
)
if args.kvm_boot:
    mode += " (KVM boot -> switch both -> ATOMIC workload)"
print(f"  Mode           : {mode}")
if args.kvm_boot:
    print(f"  KVM boot       : both Systems boot under KVM (disjoint event")
    print(f"                   queues); guests park in heartbeat/poll loops.")
    print(f"                   Boot barrier ('host boot OK' AND 'device boot")
    print(f"                   OK') -> switch BOTH to ATOMIC together ->")
    print(f"                   phase-2 workload injected via readfile.")
    print(f"                   Terminates on '{PASS_HOST}' AND '{PASS_DEV}'.")
if args.dual_kvm:
    print(f"  Dual-KVM       : both Systems on KVM cores, event queues")
    print(
        f"                   renumbered disjoint (see [dual-kvm] line above)."
    )
    print(f"                   Success = both serials show kernel boot within")
    print(f"                   ~1 min and both PASS strings in ~2-5 min.")
    print(
        f"                   Attempt-A repro = serials still empty after ~3 min."
    )
if args.offload:
    print(
        f"  Offload        : host_offload -> CXL mailbox @ 0x2ff000000 -> device_offload"
    )
    print(
        f"  Host carve-out : memmap=16M$0x2ff000000 (Reserved, /dev/mem-mmappable)"
    )
    if args.device_handoff:
        _ho_base = device_board._cxl_handoff_start
        _ho_size = device_board._cxl_handoff_size
        print(
            f"  Handoff region : [{_ho_base:#x}..{_ho_base + _ho_size:#x}) "
            f"(protected; verifier re-roots)"
        )
        print(
            f"  Handoff carve  : memmap={_ho_size >> 20}M${_ho_base:#x} "
            f"(host Reserved; operands live at {_ho_base + 0x1000:#x})"
        )
        print(
            f"  Data binding   : blob in mailbox (control), XTEA operands in"
        )
        print(
            f"                   the handed-off PROTECTED region (data path);"
        )
        print(
            f"                   ciphertext match proves the device computed"
        )
        print(f"                   on data read out of the protected region.")
    print(
        f"  Success        : 'HOST OFFLOAD ... OK' AND 'DEVICE OFFLOAD done ...'"
    )
    print(f"  NOTE           : barrier also ends on failure — every exit path")
    print(
        f"                   prints HOST/DEVICE OFFLOAD (OK/FAIL/TIMEOUT/ERROR)."
    )
    print(
        f"                   Requires host_offload + device_offload installed"
    )
    print(
        f"                   in the guest image (benchmarks/Makefile install)."
    )
if args.take_checkpoint:
    print(f"  Checkpoint dir : {args.take_checkpoint}")
    print(f"  Barrier extra  : device must heartbeat 'CXL={_CXL_SENTINEL}'")
    print(f"                   BEFORE the checkpoint is written — a broken")
    print(f"                   sentinel write/read cannot poison the ckpt.")
    print(f"  NOTE           : full validation is the RESTORE run reading")
    print(f"                   the sentinel back from CXL DRAM (and the")
    print(f"                   host surviving, post recvFunctional fix).")
if args.restore:
    print(f"  Restoring from : {args.restore}")
    print(f"  Success        : 'host alive' AND 'CXL={_CXL_SENTINEL}' in")
    print(f"                   the FRESH outdir serial files")
    print(f"  NOTE           : validates restored CXL contents via the")
    print(f"                   device-path sentinel readback; the host")
    print(f"                   functional-write path is validated by host")
    print(f"                   survival (its node-1 page tables must have")
    print(f"                   round-tripped for it to run at all).")
print("=" * 80)

simulator.run()
