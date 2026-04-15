#include "dev/storage/cxl_nmp_device.hh"

#include <algorithm>
#include <cstring>

#include "base/chunk_generator.hh"
#include "base/trace.hh"
#include "debug/CXLNMPDevice.hh"
#include "mem/packet_access.hh"

namespace gem5
{

CXLNMPDevice::CXLNMPDevice(const Params &p)
    : PciDevice(p),
      currentState(STATE_IDLE),
      dataBuffer(nullptr),
      inputAddr(0),
      outputAddr(0),
      dataSize(0),
      opcode(0),
      readBytesTransferred(0),
      writeBytesTransferred(0),
      nextReadAddr(0),
      nextWriteAddr(0),
      retryEvent([this]{ /* Retry logic if needed */ }, p.name + ".retryEvent"),
      startOpEvent([this]{ startOperation(); }, p.name + ".startOp"),
      memPort(p.name + ".mem_port", *this)
{
    // Initialize all registers to zero
    for (int i = 0; i < 8; i++) {
        registers[i] = 0;
    }

    // Set initial status to DONE (idle, ready for commands)
    registers[REG_STATUS / 8] = STATUS_DONE;

    DPRINTF(CXLNMPDevice, "CXLNMPDevice created: %s\n", name());
    DPRINTF(CXLNMPDevice, "  Register space: %d bytes (8 × 64-bit regs)\n",
            REG_SPACE_SIZE);
    DPRINTF(CXLNMPDevice, "  BAR2 size: %lu bytes\n", p.BAR2->size());
    DPRINTF(CXLNMPDevice, "  Phase 2: DMA engine enabled\n");
}

Port &
CXLNMPDevice::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_port")
        return memPort;
    else if (if_name == "dma")
        return dmaPort;
    else
        return PioDevice::getPort(if_name, idx);
}

void
CXLNMPDevice::init()
{
    PciDevice::init();

    if (!memPort.isConnected()) {
        warn("CXLNMPDevice %s mem_port is not connected!", name());
        warn("  This device requires connection to cxl_mem_bus for NMP operation");
    } else {
        DPRINTF(CXLNMPDevice, "mem_port connected successfully\n");
    }
}

AddrRangeList
CXLNMPDevice::getAddrRanges() const
{
    // PciDevice base class handles BAR address ranges
    return PciDevice::getAddrRanges();
}

Tick
CXLNMPDevice::read(PacketPtr pkt)
{
    // This is called for PIO reads to BAR2 (register space)
    // For I/O BARs: CPU → IO Bus → NMP PIO (direct I/O port access)
    // For Memory BARs: CPU → L3 → MemBus → CXL Bridge → IO Bus → NMP PIO

    // Get BAR2 base address and calculate register offset
    Addr bar2_base = BARs[2]->addr();
    Addr offset = pkt->getAddr() - bar2_base;
    assert(offset < REG_SPACE_SIZE);

    // Support both 32-bit (I/O port) and 64-bit (memory-mapped) register access
    // 32-bit I/O: offset 0x00=low32 of reg0, 0x04=high32 of reg0, 0x08=low32 of reg1, etc.
    // 64-bit MEM: offset 0x00=reg0, 0x08=reg1, etc.
    unsigned int access_size = pkt->getSize();
    if (access_size != 4 && access_size != 8) {
        panic("CXLNMPDevice: Only 32-bit or 64-bit register access supported, got %d bytes",
              access_size);
    }

    if (offset % 4 != 0) {
        panic("CXLNMPDevice: Register access must be 4-byte aligned, offset=0x%x",
              offset);
    }

    // Calculate which 64-bit register we're accessing
    uint64_t reg_index = (offset / 8);
    uint64_t reg_value = registers[reg_index];

    if (access_size == 8) {
        // 64-bit access - return full register
        pkt->setLE<uint64_t>(reg_value);
    } else {
        // 32-bit access - return low or high half based on offset
        bool is_high_half = (offset % 8) == 4;
        uint32_t value32;
        if (is_high_half) {
            value32 = (uint32_t)((reg_value >> 32) & 0xFFFFFFFF);
        } else {
            value32 = (uint32_t)(reg_value & 0xFFFFFFFF);
        }
        pkt->setLE<uint32_t>(value32);
    }

    DPRINTF(CXLNMPDevice, "Register READ: offset=0x%02x reg=%lu value=0x%016lx size=%dB (%s)\n",
            offset, reg_index, reg_value, access_size,
            (offset & ~0x4) == REG_INPUT_ADDR ? "INPUT_ADDR" :
            (offset & ~0x4) == REG_OUTPUT_ADDR ? "OUTPUT_ADDR" :
            (offset & ~0x4) == REG_DATA_SIZE ? "DATA_SIZE" :
            (offset & ~0x4) == REG_OPCODE ? "OPCODE" :
            (offset & ~0x4) == REG_CONTROL ? "CONTROL" :
            (offset & ~0x4) == REG_STATUS ? "STATUS" :
            (offset & ~0x4) == REG_RESERVED0 ? "RESERVED0" :
            (offset & ~0x4) == REG_RESERVED1 ? "RESERVED1" : "UNKNOWN");

    // Model register read latency (PIO access through PciDevice)
    // PciDevice doesn't have a configurable PIO latency parameter,
    // so we use a fixed small delay

    // CRITICAL: Convert packet to atomic response
    // PioPort::recvAtomic() expects the device to call this
    pkt->makeAtomicResponse();

    return pioDelay;
}

Tick
CXLNMPDevice::write(PacketPtr pkt)
{
    // This is called for PIO writes to BAR2 (register space)
    // For I/O BARs: CPU → IO Bus → NMP PIO (direct I/O port access)
    // For Memory BARs: CPU → L3 → MemBus → CXL Bridge → IO Bus → NMP PIO

    // Get BAR2 base address and calculate register offset
    Addr bar2_base = BARs[2]->addr();
    Addr offset = pkt->getAddr() - bar2_base;
    assert(offset < REG_SPACE_SIZE);

    // Support both 32-bit (I/O port) and 64-bit (memory-mapped) register access
    // 32-bit I/O: offset 0x00=low32 of reg0, 0x04=high32 of reg0, 0x08=low32 of reg1, etc.
    // 64-bit MEM: offset 0x00=reg0, 0x08=reg1, etc.
    unsigned int access_size = pkt->getSize();
    if (access_size != 4 && access_size != 8) {
        panic("CXLNMPDevice: Only 32-bit or 64-bit register access supported, got %d bytes",
              access_size);
    }

    if (offset % 4 != 0) {
        panic("CXLNMPDevice: Register access must be 4-byte aligned, offset=0x%x",
              offset);
    }

    // Calculate which 64-bit register we're accessing
    uint64_t reg_index = (offset / 8);
    uint64_t new_value;

    if (access_size == 8) {
        // 64-bit access - write full register
        new_value = pkt->getLE<uint64_t>();
    } else {
        // 32-bit access - update low or high half based on offset
        bool is_high_half = (offset % 8) == 4;
        uint32_t value32 = pkt->getLE<uint32_t>();
        uint64_t old_value = registers[reg_index];

        if (is_high_half) {
            // Update high 32 bits, preserve low 32 bits
            new_value = (old_value & 0x00000000FFFFFFFFULL) | ((uint64_t)value32 << 32);
        } else {
            // Update low 32 bits, preserve high 32 bits
            new_value = (old_value & 0xFFFFFFFF00000000ULL) | (uint64_t)value32;
        }
    }

    DPRINTF(CXLNMPDevice, "Register WRITE: offset=0x%02x reg=%lu value=0x%016lx size=%dB (%s)\n",
            offset, reg_index, new_value, access_size,
            (offset & ~0x4) == REG_INPUT_ADDR ? "INPUT_ADDR" :
            (offset & ~0x4) == REG_OUTPUT_ADDR ? "OUTPUT_ADDR" :
            (offset & ~0x4) == REG_DATA_SIZE ? "DATA_SIZE" :
            (offset & ~0x4) == REG_OPCODE ? "OPCODE" :
            (offset & ~0x4) == REG_CONTROL ? "CONTROL" :
            (offset & ~0x4) == REG_STATUS ? "STATUS" :
            (offset & ~0x4) == REG_RESERVED0 ? "RESERVED0" :
            (offset & ~0x4) == REG_RESERVED1 ? "RESERVED1" : "UNKNOWN");

    // Handle special register behaviors
    // For 32-bit accesses, need to check both offset and offset+4
    if ((offset & ~0x4) == REG_STATUS) {
        // Status register is read-only
        DPRINTF(CXLNMPDevice, "  Ignoring write to read-only STATUS register\n");
        pkt->makeAtomicResponse();
        return pioDelay;
    }

    // CRITICAL: Only trigger START on low-half write (offset 0x30), not high-half (0x34)
    // For 32-bit I/O: write_reg_io() sends low32 to 0x30, then high32 to 0x34
    // We only want to start the operation once, after both halves are written
    if (offset == REG_CONTROL) {
        // Control register: writing CTRL_START triggers operation
        // Store the value first
        registers[reg_index] = new_value;

        if (new_value & CTRL_START) {
            DPRINTF(CXLNMPDevice, "*** START command received! ***\n");
            DPRINTF(CXLNMPDevice, "  INPUT_ADDR  = 0x%016lx\n",
                    registers[REG_INPUT_ADDR / 8]);
            DPRINTF(CXLNMPDevice, "  OUTPUT_ADDR = 0x%016lx\n",
                    registers[REG_OUTPUT_ADDR / 8]);
            DPRINTF(CXLNMPDevice, "  DATA_SIZE   = %lu bytes\n",
                    registers[REG_DATA_SIZE / 8]);
            DPRINTF(CXLNMPDevice, "  OPCODE      = %lu\n",
                    registers[REG_OPCODE / 8]);

            // Phase 2: Schedule DMA operation for next tick
            // CRITICAL: Cannot call startOperation() directly here because
            // the CPU's write packet is still being routed through the crossbar.
            // Calling sendTimingReq() from within this handler would create
            // a re-entrant routing conflict. Schedule for next tick instead.
            DPRINTF(CXLNMPDevice, "  Scheduling operation start for tick %lu\n",
                    curTick() + 1);
            if (!startOpEvent.scheduled()) {
                schedule(startOpEvent, curTick() + 1);
            }

            // Control register is write-only and self-clearing
            // Don't store the value
            pkt->makeAtomicResponse();
            return pioDelay;
        }
    }

    // Store the register value (unless it was already stored above for CONTROL)
    registers[reg_index] = new_value;

    // CRITICAL: Convert packet to atomic response
    // PioPort::recvAtomic() expects the device to call this
    pkt->makeAtomicResponse();

    return pioDelay;
}

bool
CXLNMPDevice::NMPMemoryPort::recvTimingResp(PacketPtr pkt)
{
    // Handle memory read/write responses
    DPRINTF(CXLNMPDevice, "recvTimingResp: %s addr=0x%lx size=%d\n",
            pkt->cmdString(), pkt->getAddr(), pkt->getSize());

    if (pkt->isRead()) {
        // Route to appropriate handler based on current state
        if (device.currentState == STATE_CHASING) {
            device.handleChaseReadComplete(pkt);
        } else {
            device.handleReadComplete(pkt);
        }
    } else if (pkt->isWrite()) {
        device.handleWriteComplete(pkt);
    } else {
        panic("CXLNMPDevice: Unexpected packet type: %s", pkt->cmdString());
    }

    return true;
}

void
CXLNMPDevice::NMPMemoryPort::recvReqRetry()
{
    // Phase 2: Handle request retry
    // For now, panic - we'll implement retry logic if needed
    panic("CXLNMPDevice: recvReqRetry not yet implemented - need retry logic");
}

// ===== Phase 2: DMA Operation Implementation =====

void
CXLNMPDevice::startOperation()
{
    DPRINTF(CXLNMPDevice, "startOperation: Initiating DMA operation\n");

    // Validate state
    if (currentState != STATE_IDLE) {
        abortOperation("Device is already busy");
        return;
    }

    // Copy register values to member variables (snapshot at START time)
    inputAddr = registers[REG_INPUT_ADDR / 8];
    outputAddr = registers[REG_OUTPUT_ADDR / 8];
    dataSize = registers[REG_DATA_SIZE / 8];
    opcode = registers[REG_OPCODE / 8];

    // Validate common parameters
    if (inputAddr == 0 || dataSize == 0) {
        abortOperation("Invalid operation parameters (zero input address or size)");
        return;
    }

    // Route to operation-specific handler
    if (opcode == OP_MEMCPY) {
        // Phase 2: Memcpy operation
        // For memcpy, outputAddr is required
        if (outputAddr == 0) {
            abortOperation("Memcpy: output address cannot be zero");
            return;
        }

        // Allocate data buffer
        dataBuffer = new uint8_t[dataSize];
        if (!dataBuffer) {
            abortOperation("Failed to allocate data buffer");
            return;
        }

        // Reset transfer tracking
        readBytesTransferred = 0;
        writeBytesTransferred = 0;
        nextReadAddr = inputAddr;
        nextWriteAddr = outputAddr;

        // Set status to BUSY
        registers[REG_STATUS / 8] = STATUS_BUSY;

        DPRINTF(CXLNMPDevice, "  Memcpy started: %lu bytes from 0x%lx to 0x%lx\n",
                dataSize, inputAddr, outputAddr);

        // Begin read phase
        currentState = STATE_READING;
        startReadPhase();

    } else if (opcode == OP_PTR_CHASE) {
        // Phase 3A: Pointer chase operation
        // INPUT_ADDR = starting address
        // DATA_SIZE = number of hops
        // OUTPUT_ADDR = where to write final address (0 = don't write)
        // RESERVED0 = pointer offset within each block
        ptrOffset = registers[REG_RESERVED0 / 8];

        // Validate parameters
        if (dataSize == 0) {
            abortOperation("Pointer chase: hop count cannot be zero");
            return;
        }
        if (ptrOffset > 4096 - 8) {  // Pointer must fit in reasonable block
            abortOperation("Pointer chase: pointer offset too large");
            return;
        }

        // Initialize chase state
        currentChaseAddr = inputAddr;
        hopsRemaining = dataSize;  // DATA_SIZE = number of hops
        hopsCompleted = 0;

        // Set status to BUSY
        registers[REG_STATUS / 8] = STATUS_BUSY;
        currentState = STATE_CHASING;

        DPRINTF(CXLNMPDevice, "  Pointer chase started: %lu hops from 0x%lx "
                "(ptr offset %lu)\n",
                hopsRemaining, currentChaseAddr, ptrOffset);

        startPointerChase();

    } else {
        abortOperation("Unsupported opcode");
        return;
    }
}

void
CXLNMPDevice::startReadPhase()
{
    DPRINTF(CXLNMPDevice, "startReadPhase: Reading %lu bytes from 0x%lx\n",
            dataSize, inputAddr);

    // Send initial pipeline of requests (up to maxOutstandingReqs)
    for (int i = 0; i < maxOutstandingReqs && readBytesTransferred < dataSize; i++) {
        sendNextReadChunk();
    }
}

void
CXLNMPDevice::sendNextReadChunk()
{
    // Check if we're done reading
    if (readBytesTransferred >= dataSize) {
        return;
    }

    // Calculate chunk size (64 bytes, or less for the last chunk)
    const Addr chunkSize = 64;
    Addr remaining = dataSize - readBytesTransferred;
    Addr this_chunk_size = std::min(chunkSize, remaining);

    DPRINTF(CXLNMPDevice, "  sendNextReadChunk: addr=0x%lx size=%lu (total %lu/%lu)\n",
            nextReadAddr, this_chunk_size, readBytesTransferred, dataSize);

    // Create read request
    RequestPtr req = std::make_shared<Request>(
        nextReadAddr, this_chunk_size, 0, Request::funcRequestorId);

    // Create packet with data buffer
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    // Send request through mem_port (direct to cxl_mem_bus)
    if (!memPort.sendTimingReq(pkt)) {
        // Request failed - need retry logic
        panic("CXLNMPDevice: sendTimingReq failed - retry not implemented");
    }

    // Track pending response
    pendingReadResponses.push_back(pkt);

    // Update for next chunk
    nextReadAddr += this_chunk_size;
    readBytesTransferred += this_chunk_size;
}

void
CXLNMPDevice::handleReadComplete(PacketPtr pkt)
{
    DPRINTF(CXLNMPDevice, "handleReadComplete: addr=0x%lx size=%d\n",
            pkt->getAddr(), pkt->getSize());

    // Check if operation was aborted
    if (currentState == STATE_ERROR) {
        DPRINTF(CXLNMPDevice, "  Operation aborted - discarding in-flight read response\n");
        delete pkt;
        return;
    }

    // Verify we're in the correct state
    if (currentState != STATE_READING) {
        panic("CXLNMPDevice: Received read response in state %d (expected READING)",
              currentState);
    }

    // Calculate offset into data buffer
    Addr offset = pkt->getAddr() - inputAddr;

    // Copy data from packet to buffer
    std::memcpy(dataBuffer + offset, pkt->getConstPtr<uint8_t>(), pkt->getSize());

    DPRINTF(CXLNMPDevice, "  Copied %d bytes to buffer offset %lu\n",
            pkt->getSize(), offset);

    // Remove from pending queue
    auto it = std::find(pendingReadResponses.begin(),
                        pendingReadResponses.end(), pkt);
    if (it != pendingReadResponses.end()) {
        pendingReadResponses.erase(it);
    }

    // Free the packet
    delete pkt;

    // Send next read chunk to maintain pipeline (if more data to read)
    // Note: readBytesTransferred was already incremented in sendNextReadChunk()
    if (readBytesTransferred < dataSize) {
        sendNextReadChunk();
    }

    // Check if all reads are complete (all sent AND all received)
    if (readBytesTransferred >= dataSize && pendingReadResponses.empty()) {
        DPRINTF(CXLNMPDevice, "  Read phase complete (%lu bytes), transitioning to WRITING\n",
                readBytesTransferred);

        // For memcpy, processing is a no-op (data already in buffer)
        currentState = STATE_PROCESSING;
        DPRINTF(CXLNMPDevice, "  Processing complete (memcpy = no-op)\n");

        // Reset for write phase
        readBytesTransferred = 0;  // Reuse for write tracking
        writeBytesTransferred = 0;

        // Begin write phase
        currentState = STATE_WRITING;
        startWritePhase();
    }
}

void
CXLNMPDevice::startWritePhase()
{
    DPRINTF(CXLNMPDevice, "startWritePhase: Writing %lu bytes to 0x%lx\n",
            dataSize, outputAddr);

    // Send initial pipeline of requests (up to maxOutstandingReqs)
    for (int i = 0; i < maxOutstandingReqs && writeBytesTransferred < dataSize; i++) {
        sendNextWriteChunk();
    }
}

void
CXLNMPDevice::sendNextWriteChunk()
{
    // Check if we're done writing
    if (writeBytesTransferred >= dataSize) {
        return;
    }

    // Calculate chunk size (64 bytes, or less for the last chunk)
    const Addr chunkSize = 64;
    Addr remaining = dataSize - writeBytesTransferred;
    Addr this_chunk_size = std::min(chunkSize, remaining);

    // Calculate offset into data buffer
    Addr offset = nextWriteAddr - outputAddr;

    DPRINTF(CXLNMPDevice, "  sendNextWriteChunk: addr=0x%lx size=%lu (total %lu/%lu)\n",
            nextWriteAddr, this_chunk_size, writeBytesTransferred, dataSize);

    // Create write request
    RequestPtr req = std::make_shared<Request>(
        nextWriteAddr, this_chunk_size, 0, Request::funcRequestorId);

    // Create packet with data from buffer
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    std::memcpy(pkt->getPtr<uint8_t>(), dataBuffer + offset, this_chunk_size);

    // Send request through mem_port (direct to cxl_mem_bus)
    if (!memPort.sendTimingReq(pkt)) {
        // Request failed - need retry logic
        panic("CXLNMPDevice: sendTimingReq failed - retry not implemented");
    }

    // Track pending response
    pendingWriteResponses.push_back(pkt);

    // Update for next chunk
    nextWriteAddr += this_chunk_size;
    writeBytesTransferred += this_chunk_size;
}

void
CXLNMPDevice::handleWriteComplete(PacketPtr pkt)
{
    DPRINTF(CXLNMPDevice, "handleWriteComplete: addr=0x%lx size=%d\n",
            pkt->getAddr(), pkt->getSize());

    // Check if operation was aborted
    if (currentState == STATE_ERROR) {
        DPRINTF(CXLNMPDevice, "  Operation aborted - discarding in-flight write response\n");
        delete pkt;
        return;
    }

    // Verify we're in a valid state for write completion
    if (currentState != STATE_WRITING && currentState != STATE_CHASING) {
        panic("CXLNMPDevice: Received write response in state %d (expected WRITING or CHASING)",
              currentState);
    }

    // Remove from pending queue
    auto it = std::find(pendingWriteResponses.begin(),
                        pendingWriteResponses.end(), pkt);
    if (it != pendingWriteResponses.end()) {
        pendingWriteResponses.erase(it);
    }

    // Free the packet
    delete pkt;

    // Handle based on operation type
    if (currentState == STATE_CHASING) {
        // Pointer chase: single write of final address - we're done
        DPRINTF(CXLNMPDevice, "  Final address write complete, operation DONE\n");
        completeOperation();
    } else {
        // Memcpy: bulk write phase - continue pipeline
        // Send next write chunk to maintain pipeline (if more data to write)
        // Note: writeBytesTransferred was already incremented in sendNextWriteChunk()
        if (writeBytesTransferred < dataSize) {
            sendNextWriteChunk();
        }

        // Check if all writes are complete (all sent AND all received)
        if (writeBytesTransferred >= dataSize && pendingWriteResponses.empty()) {
            DPRINTF(CXLNMPDevice, "  Write phase complete (%lu bytes), operation DONE\n",
                    writeBytesTransferred);
            completeOperation();
        }
    }
}

void
CXLNMPDevice::completeOperation()
{
    DPRINTF(CXLNMPDevice, "completeOperation: Operation completed successfully\n");

    // Free data buffer
    if (dataBuffer) {
        delete[] dataBuffer;
        dataBuffer = nullptr;
    }

    // Set status to DONE
    registers[REG_STATUS / 8] = STATUS_DONE;

    // Return to idle state
    currentState = STATE_IDLE;

    DPRINTF(CXLNMPDevice, "  Device ready for next operation\n");
}

void
CXLNMPDevice::abortOperation(const char *reason)
{
    warn("CXLNMPDevice: Aborting operation - %s", reason);

    // Free data buffer if allocated
    if (dataBuffer) {
        delete[] dataBuffer;
        dataBuffer = nullptr;
    }

    // Clear pending queues
    // CRITICAL: Don't delete packets here - they may have in-flight responses
    // in the memory system that will call handleReadComplete/handleWriteComplete
    // Those handlers will check for STATE_ERROR and delete the packets
    pendingReadResponses.clear();
    pendingWriteResponses.clear();

    // Set status to ERROR
    registers[REG_STATUS / 8] = STATUS_ERROR;

    // Return to error state
    currentState = STATE_ERROR;

    DPRINTF(CXLNMPDevice, "  Device in ERROR state\n");
}

// ===== Phase 3A: Pointer Chase Implementation =====

void
CXLNMPDevice::startPointerChase()
{
    DPRINTF(CXLNMPDevice, "startPointerChase: Reading node at 0x%lx "
            "(hop %lu/%lu)\n",
            currentChaseAddr, hopsCompleted + 1,
            hopsCompleted + hopsRemaining);

    sendChaseRead();
}

void
CXLNMPDevice::sendChaseRead()
{
    // Read one cache line (64 bytes) containing the pointer
    const Addr chunkSize = 64;

    DPRINTF(CXLNMPDevice, "  sendChaseRead: addr=0x%lx size=%lu\n",
            currentChaseAddr, chunkSize);

    // Create read request for current node
    RequestPtr req = std::make_shared<Request>(
        currentChaseAddr, chunkSize, 0, Request::funcRequestorId);

    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    // Send through mem_port (direct to cxl_mem_bus)
    if (!memPort.sendTimingReq(pkt)) {
        panic("CXLNMPDevice: sendTimingReq failed in pointer chase");
    }

    // Track this request
    pendingReadResponses.push_back(pkt);
}

void
CXLNMPDevice::handleChaseReadComplete(PacketPtr pkt)
{
    DPRINTF(CXLNMPDevice, "handleChaseReadComplete: addr=0x%lx size=%d\n",
            pkt->getAddr(), pkt->getSize());

    // Check if operation was aborted
    if (currentState == STATE_ERROR) {
        DPRINTF(CXLNMPDevice, "  Operation aborted - discarding in-flight "
                "chase read response\n");
        delete pkt;
        return;
    }

    // Extract pointer at specified offset
    const uint8_t* data = pkt->getConstPtr<uint8_t>();
    uint64_t nextPtr;
    std::memcpy(&nextPtr, data + ptrOffset, sizeof(uint64_t));

    DPRINTF(CXLNMPDevice, "  Extracted pointer: 0x%lx (offset %lu in block)\n",
            nextPtr, ptrOffset);

    // Remove from pending queue
    auto it = std::find(pendingReadResponses.begin(),
                        pendingReadResponses.end(), pkt);
    if (it != pendingReadResponses.end()) {
        pendingReadResponses.erase(it);
    }
    delete pkt;

    // Update state
    hopsCompleted++;
    hopsRemaining--;
    currentChaseAddr = nextPtr;

    // Check if we're done
    if (hopsRemaining == 0) {
        DPRINTF(CXLNMPDevice, "  Pointer chase complete: %lu hops, "
                "final address 0x%lx\n",
                hopsCompleted, currentChaseAddr);

        // Optionally write final address to OUTPUT_ADDR
        if (outputAddr != 0) {
            DPRINTF(CXLNMPDevice, "  Writing final address to 0x%lx\n",
                    outputAddr);

            // Create write request
            RequestPtr req = std::make_shared<Request>(
                outputAddr, sizeof(uint64_t), 0, Request::funcRequestorId);
            PacketPtr wpkt = new Packet(req, MemCmd::WriteReq);
            wpkt->allocate();
            std::memcpy(wpkt->getPtr<uint8_t>(), &currentChaseAddr,
                        sizeof(uint64_t));

            if (!memPort.sendTimingReq(wpkt)) {
                panic("CXLNMPDevice: sendTimingReq failed writing result");
            }
            pendingWriteResponses.push_back(wpkt);
            // Will call completeOperation() in handleWriteComplete()
        } else {
            // No result write needed
            completeOperation();
        }
    } else {
        // Continue chasing
        sendChaseRead();
    }
}

} // namespace gem5
