from m5.objects.PciDevice import *
from m5.params import *


class CXLNMPDevice(PciDevice):
    type = "CXLNMPDevice"
    cxx_header = "dev/storage/cxl_nmp_device.hh"
    cxx_class = "gem5::CXLNMPDevice"

    # Memory port for direct CXL DRAM access (data path - no CXL Bridge overhead)
    mem_port = RequestPort(
        "This port sends requests directly to cxl_mem_bus (bypasses CXL Bridge)"
    )

    # PCI Configuration
    # Use fake vendor/device IDs to avoid Linux driver matching
    VendorID = 0x1234  # Fake vendor ID (not a real company)
    DeviceID = 0x0001  # NMP device type
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05  # Memory controller
    SubClassCode = 0x80  # Other memory controller
    ProgIF = 0x00
    InterruptLine = 0x1F
    InterruptPin = 0x01

    # BAR0 and BAR1 are reserved for future use (could map CXL memory range here)
    # BAR2 is used for NMP control registers (64 bytes, 8 × 64-bit registers)
    # Use I/O BAR instead of memory BAR to avoid conflict with iocache
    BAR0 = PciBarNone()
    BAR1 = PciBarNone()
    BAR2 = PciLegacyIoBar(
        addr=0x400, size="64B"
    )  # NMP register space (I/O ports 0x400-0x43F)
    BAR3 = PciBarNone()
    BAR4 = PciBarNone()
    BAR5 = PciBarNone()
