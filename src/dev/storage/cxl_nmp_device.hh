#ifndef __DEV_STORAGE_CXL_NMP_DEVICE_HH__
#define __DEV_STORAGE_CXL_NMP_DEVICE_HH__

#include <deque>

#include "base/addr_range.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "dev/pci/device.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/CXLNMPDevice.hh"

namespace gem5
{

/**
 * CXL Near-Memory Processing (NMP) Device
 *
 * This device models a simple fixed-function processor embedded inside
 * a CXL memory expander. It provides:
 *
 * 1. PIO interface (BAR2): 8 × 64-bit control registers accessible from
 *    the host CPU through the CXL bridge (control path - has CXL overhead)
 *
 * 2. Memory port: Direct connection to cxl_mem_bus for local DRAM access
 *    (data path - bypasses CXL bridge, no protocol overhead)
 *
 * The CPU offloads work by writing to PIO registers. The NMP device
 * autonomously reads data from CXL DRAM, processes it, writes results
 * back, and updates status.
 */
class CXLNMPDevice : public PciDevice
{
  public:
    // Register offsets (64-bit registers, 8-byte aligned)
    enum RegisterOffset : Addr
    {
        REG_INPUT_ADDR   = 0x00,  // Input buffer physical address (64-bit)
        REG_OUTPUT_ADDR  = 0x08,  // Output buffer physical address (64-bit)
        REG_DATA_SIZE    = 0x10,  // Data size in bytes (64-bit)
        REG_OPCODE       = 0x18,  // Operation code (0=memcpy, others reserved)
        REG_RESERVED0    = 0x20,  // Reserved for future use
        REG_RESERVED1    = 0x28,  // Reserved for future use
        REG_CONTROL      = 0x30,  // Control register (write to start)
        REG_STATUS       = 0x38,  // Status register (0=busy, 1=done)

        // Total register space size
        REG_SPACE_SIZE   = 0x40   // 64 bytes
    };

    // Control register bits
    enum ControlBits : uint64_t
    {
        CTRL_START = 0x1  // Bit 0: Start operation (self-clearing)
    };

    // Status register bits
    enum StatusBits : uint64_t
    {
        STATUS_BUSY  = 0x0,  // Device is busy processing
        STATUS_DONE  = 0x1,  // Operation completed successfully
        STATUS_ERROR = 0x2   // Error occurred during operation
    };

    // Operation codes
    enum Opcode : uint64_t
    {
        OP_MEMCPY = 0x0,     // Simple memory copy (Phase 2)
        OP_PTR_CHASE = 0x1   // Pointer chasing (Phase 3A)
        // Future: OP_SEARCH, OP_REDUCE, OP_SCAN, OP_FILTER, etc.
    };

    // State machine for DMA operations
    enum OperationState : uint8_t
    {
        STATE_IDLE,        // No operation in progress
        STATE_READING,     // Memcpy: bulk reading data from INPUT_ADDR
        STATE_PROCESSING,  // Memcpy: processing data (memcpy = no-op)
        STATE_WRITING,     // Memcpy: bulk writing data to OUTPUT_ADDR
        STATE_CHASING,     // Pointer chase: iterative chase state (Phase 3A)
        STATE_ERROR        // Error occurred
    };

  protected:
    /** Register storage (8 × 64-bit registers) */
    uint64_t registers[8];

    /** Phase 2: DMA operation state */
    OperationState currentState;

    /** Phase 2: Data buffer for temporary storage during DMA */
    uint8_t *dataBuffer;

    /** Phase 2: Operation parameters (copied from registers at START) */
    Addr inputAddr;
    Addr outputAddr;
    Addr dataSize;
    uint64_t opcode;

    /** Phase 2: Transfer tracking */
    Addr readBytesTransferred;
    Addr writeBytesTransferred;
    Addr nextReadAddr;   // Next address to read from
    Addr nextWriteAddr;  // Next address to write to

    /** Phase 2: Outstanding request tracking */
    std::deque<PacketPtr> pendingReadResponses;
    std::deque<PacketPtr> pendingWriteResponses;
    static const int maxOutstandingReqs = 1;  // Pipeline depth (conservative for now)

    /** Phase 3A: Pointer chase state */
    Addr currentChaseAddr;     // Current node address being read
    Addr ptrOffset;            // Offset within node where pointer is stored
    uint64_t hopsRemaining;    // Hops left to perform
    uint64_t hopsCompleted;    // Hops completed so far

    /** Phase 2: Events */
    EventFunctionWrapper retryEvent;
    EventFunctionWrapper startOpEvent;  // Deferred operation start

    /** Request port for direct CXL memory access (data path) */
    class NMPMemoryPort : public RequestPort
    {
      private:
        CXLNMPDevice& device;

      public:
        NMPMemoryPort(const std::string& name, CXLNMPDevice& dev)
            : RequestPort(name), device(dev)
        { }

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    /** Memory port for accessing CXL DRAM */
    NMPMemoryPort memPort;

    /**
     * Handle register read access (from host CPU via PIO)
     * This goes through CXL Bridge - expected to have protocol overhead
     */
    Tick read(PacketPtr pkt) override;

    /**
     * Handle register write access (from host CPU via PIO)
     * This goes through CXL Bridge - expected to have protocol overhead
     */
    Tick write(PacketPtr pkt) override;

    /** Phase 2: DMA operation helpers */
    void startOperation();                   // Initiate DMA operation
    void startReadPhase();                   // Begin reading from INPUT_ADDR
    void sendNextReadChunk();                // Send one read request (flow control)
    void startWritePhase();                  // Begin writing to OUTPUT_ADDR
    void sendNextWriteChunk();               // Send one write request (flow control)
    void handleReadComplete(PacketPtr pkt);  // Process read response
    void handleWriteComplete(PacketPtr pkt); // Process write response
    void completeOperation();                // Mark operation as DONE
    void abortOperation(const char *reason); // Abort with error

    /** Phase 3A: Pointer chase helpers */
    void startPointerChase();                      // Begin pointer chase operation
    void sendChaseRead();                          // Send read for current node
    void handleChaseReadComplete(PacketPtr pkt);   // Extract pointer and continue

  public:
    PARAMS(CXLNMPDevice);
    CXLNMPDevice(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    void init() override;

    AddrRangeList getAddrRanges() const override;
};

} // namespace gem5

#endif // __DEV_STORAGE_CXL_NMP_DEVICE_HH__
