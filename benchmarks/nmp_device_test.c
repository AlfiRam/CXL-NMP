/**
 * CXL NMP Device Test Program
 *
 * Tests the CXLNMPDevice DMA engine by performing a simple memcpy operation.
 *
 * What this does:
 * 1. Discovers NMP device BAR2 address (register space) from sysfs
 * 2. Maps BAR2 into userspace for register access via /dev/mem
 * 3. Allocates CXL memory buffers using MAP_ANONYMOUS (normal allocation)
 * 4. Translates virtual addresses to physical using /proc/self/pagemap
 * 5. Programs NMP device with PHYSICAL addresses to copy data
 * 6. Verifies the copy succeeded
 *
 * CRITICAL: Virtual vs Physical Addresses
 * - CPU accesses memory through VIRTUAL addresses (MMU translates, cache coherent)
 * - NMP device accesses memory through PHYSICAL addresses (direct to cxl_mem_bus)
 * - We use MAP_ANONYMOUS for buffers (normal cached memory allocation)
 * - We use /proc/self/pagemap to translate virtual → physical addresses
 * - src_buf/dst_buf = virtual addresses (for CPU to read/write)
 * - src_phys/dst_phys = physical addresses (obtained via pagemap, for NMP registers)
 *
 * Memory Layout:
 * - Source buffer:      Virtual TBD, Physical from pagemap
 * - Destination buffer: Virtual TBD, Physical from pagemap
 * - NMP device BAR2:    Discovered from /sys/bus/pci/devices/0000:00:07.0/resource
 *
 * Expected result: Source and destination buffers should match after NMP operation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <unistd.h>

#include <gem5/m5ops.h>

// NMP Device Register Offsets (from cxl_nmp_device.hh)
#define REG_INPUT_ADDR   0x00
#define REG_OUTPUT_ADDR  0x08
#define REG_DATA_SIZE    0x10
#define REG_OPCODE       0x18
#define REG_RESERVED0    0x20
#define REG_RESERVED1    0x28
#define REG_CONTROL      0x30
#define REG_STATUS       0x38

// Register Values
#define OP_MEMCPY        0x0
#define CTRL_START       0x1
#define STATUS_BUSY      0x0
#define STATUS_DONE      0x1
#define STATUS_ERROR     0x2

// Memory configuration
#define CXL_BASE_ADDR    0x100000000ULL  // 4GB - CXL memory starts here
#define TEST_SIZE        4096             // 4KB test buffer
#define BAR2_SIZE        64               // NMP register space size

// PCI device location
#define NMP_PCI_DEVICE   "0000:00:07.0"

// Page size for virtual→physical translation
#define PAGE_SIZE        4096
#define PAGEMAP_ENTRY_SIZE 8
#define PFN_MASK         ((1ULL << 55) - 1)
#define PAGE_PRESENT     (1ULL << 63)

/**
 * Translate virtual address to physical address using /proc/self/pagemap
 * Returns 0 on error
 */
uint64_t virt_to_phys(void *virt_addr)
{
    int fd;
    uint64_t page_offset, pfn, phys_addr;
    uint64_t pagemap_entry;

    // Open pagemap file
    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open /proc/self/pagemap: %s\n", strerror(errno));
        return 0;
    }

    // Calculate offset in pagemap file
    // Each page has an 8-byte entry
    page_offset = ((uint64_t)virt_addr / PAGE_SIZE) * PAGEMAP_ENTRY_SIZE;

    // Seek to the entry
    if (lseek(fd, page_offset, SEEK_SET) != (off_t)page_offset) {
        fprintf(stderr, "ERROR: Cannot seek in pagemap: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    // Read the entry
    if (read(fd, &pagemap_entry, PAGEMAP_ENTRY_SIZE) != PAGEMAP_ENTRY_SIZE) {
        fprintf(stderr, "ERROR: Cannot read pagemap entry: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    close(fd);

    // Check if page is present
    if (!(pagemap_entry & PAGE_PRESENT)) {
        fprintf(stderr, "ERROR: Page not present for virtual address %p\n", virt_addr);
        return 0;
    }

    // Extract PFN (Page Frame Number)
    pfn = pagemap_entry & PFN_MASK;

    // Calculate physical address
    // Physical = (PFN * PAGE_SIZE) + offset_within_page
    phys_addr = (pfn * PAGE_SIZE) + ((uint64_t)virt_addr % PAGE_SIZE);

    return phys_addr;
}

/**
 * Enable PCI I/O Space access for the NMP device
 *
 * The PCI Command Register (offset 0x04, 16-bit) controls device access:
 * - Bit 0: I/O Space Enable (what we need for I/O BARs)
 * - Bit 1: Memory Space Enable
 * - Bit 2: Bus Master Enable
 *
 * Returns 0 on success, -1 on failure
 */
int enable_pci_io_space(void)
{
    char config_path[256];
    int fd;
    uint16_t cmd_reg;

    snprintf(config_path, sizeof(config_path),
             "/sys/bus/pci/devices/%s/config", NMP_PCI_DEVICE);

    // Open PCI config space for read/write
    fd = open(config_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", config_path, strerror(errno));
        fprintf(stderr, "  Try running with sudo or check device permissions\n");
        return -1;
    }

    // Read current Command Register (offset 0x04, 16-bit)
    if (lseek(fd, 0x04, SEEK_SET) != 0x04) {
        fprintf(stderr, "ERROR: Cannot seek to PCI Command Register: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (read(fd, &cmd_reg, sizeof(cmd_reg)) != sizeof(cmd_reg)) {
        fprintf(stderr, "ERROR: Cannot read PCI Command Register: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("  PCI Command Register before: 0x%04x\n", cmd_reg);

    // Check if I/O Space is already enabled
    if (cmd_reg & 0x01) {
        printf("  ✓ I/O Space already enabled\n");
        close(fd);
        return 0;
    }

    // Set bit 0 (I/O Space Enable)
    cmd_reg |= 0x01;

    // Write back to Command Register
    if (lseek(fd, 0x04, SEEK_SET) != 0x04) {
        fprintf(stderr, "ERROR: Cannot seek to PCI Command Register for write: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    if (write(fd, &cmd_reg, sizeof(cmd_reg)) != sizeof(cmd_reg)) {
        fprintf(stderr, "ERROR: Cannot write PCI Command Register: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("  PCI Command Register after:  0x%04x\n", cmd_reg);
    printf("  ✓ I/O Space enabled (bit 0 set)\n");

    close(fd);
    return 0;
}

/**
 * Parse BAR2 I/O port address from /sys/bus/pci/devices/0000:00:07.0/resource
 *
 * For I/O BARs, the address is an I/O port number (e.g., 0x400), not a memory address.
 * Format: Each line is "start end flags" for BAR0, BAR1, BAR2, ...
 *
 * Returns I/O port base address (0x400) or 0 on error
 */
uint16_t get_bar2_ioport(void)
{
    FILE *fp;
    char path[256];
    uint64_t start, end, flags;
    int bar_num = 0;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", NMP_PCI_DEVICE);
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "  NMP device may not be enumerated by Linux\n");
        return 0;
    }

    // Read each line (BAR0, BAR1, BAR2, ...)
    while (fscanf(fp, "%lx %lx %lx\n", &start, &end, &flags) == 3) {
        if (bar_num == 2) {  // BAR2
            fclose(fp);
            if (start == 0) {
                fprintf(stderr, "ERROR: BAR2 is not assigned (start=0)\n");
                return 0;
            }
            printf("[BAR2] I/O Port: 0x%lx, Size: %lu bytes\n", start, end - start + 1);
            return (uint16_t)start;  // I/O port addresses are 16-bit
        }
        bar_num++;
    }

    fclose(fp);
    fprintf(stderr, "ERROR: Could not find BAR2 in resource file\n");
    return 0;
}

/**
 * Write a 64-bit register via I/O ports
 *
 * x86 doesn't have native 64-bit I/O port instructions, so we split
 * each 64-bit register into two 32-bit I/O port writes.
 *
 * @param iobase Base I/O port address (e.g., 0x400)
 * @param offset Register offset (e.g., REG_INPUT_ADDR = 0x00)
 * @param value 64-bit value to write
 */
static inline void write_reg_io(uint16_t iobase, int offset, uint64_t value)
{
    uint16_t port = iobase + offset;
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)((value >> 32) & 0xFFFFFFFF);

    // Write low 32 bits first, then high 32 bits
    outl(low, port);
    outl(high, port + 4);
}

/**
 * Read a 64-bit register via I/O ports
 *
 * x86 doesn't have native 64-bit I/O port instructions, so we split
 * each 64-bit register into two 32-bit I/O port reads.
 *
 * @param iobase Base I/O port address (e.g., 0x400)
 * @param offset Register offset (e.g., REG_STATUS = 0x38)
 * @return 64-bit value read from register
 */
static inline uint64_t read_reg_io(uint16_t iobase, int offset)
{
    uint16_t port = iobase + offset;
    uint32_t low = inl(port);
    uint32_t high = inl(port + 4);

    return ((uint64_t)high << 32) | (uint64_t)low;
}

int main(void)
{
    uint16_t bar2_ioport;        // BAR2 I/O port base address
    uint8_t *src_buf, *dst_buf;  // Virtual addresses for CPU access
    uint64_t src_phys, dst_phys;  // Physical addresses for NMP device
    int i, errors = 0;
    uint64_t status;
    int poll_count = 0;

    printf("==========================================================\n");
    printf("  CXL NMP Device Test - Phase 2 DMA Engine Verification\n");
    printf("==========================================================\n\n");

    // Step 0: Enable PCI I/O Space (CRITICAL - device starts disabled!)
    printf("[0/6] Enabling PCI I/O Space for NMP device...\n");
    if (enable_pci_io_space() != 0) {
        fprintf(stderr, "FAILED: Cannot enable PCI I/O Space\n");
        fprintf(stderr, "  The device BAR will remain [disabled] and inaccessible\n");
        return 1;
    }
    printf("\n");

    // Step 1: Use hardcoded I/O port address
    // PciLegacyIoBar has a fixed address (0x400) - no need to discover from sysfs
    printf("[1/6] Using NMP device BAR2 I/O port...\n");
    bar2_ioport = 0x400;  // Hardcoded in CXLNMPDevice.py: PciLegacyIoBar(addr=0x400)
    printf("  BAR2 I/O port: 0x%x (legacy fixed address)\n", bar2_ioport);
    printf("  Register range: 0x%x - 0x%x (64 bytes)\n\n",
           bar2_ioport, bar2_ioport + 0x3F);

    // Step 2: Get I/O port access privileges
    printf("[2/6] Requesting I/O port access privileges...\n");
    if (iopl(3) != 0) {
        fprintf(stderr, "ERROR: Cannot get I/O port access: %s\n", strerror(errno));
        fprintf(stderr, "  Try running as root\n");
        return 1;
    }
    printf("  ✓ I/O port access granted (iopl=3)\n");
    printf("  Can now access I/O ports 0x%x-0x%x\n\n", bar2_ioport, bar2_ioport + 0x3F);

    // Step 3: Allocate CXL memory buffers using MAP_ANONYMOUS
    printf("[3/6] Allocating CXL memory buffers (normal allocation)...\n");
    printf("  Using MAP_ANONYMOUS (cache coherent, normal memory management)\n");
    printf("  Will translate virtual→physical using /proc/self/pagemap\n\n");

    // Allocate source buffer using MAP_ANONYMOUS
    // This creates normal virtual memory that will be allocated from CXL NUMA node
    // if run with numactl --membind=1
    src_buf = (uint8_t *)mmap(NULL,  // Let kernel choose virtual address
                               TEST_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS,  // Normal anonymous memory
                               -1,  // No file descriptor
                               0);  // No offset
    if (src_buf == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate source buffer: %s\n", strerror(errno));
        return 1;
    }

    // Allocate destination buffer using MAP_ANONYMOUS
    dst_buf = (uint8_t *)mmap(NULL,  // Let kernel choose virtual address
                               TEST_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS,  // Normal anonymous memory
                               -1,  // No file descriptor
                               0);  // No offset
    if (dst_buf == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate destination buffer: %s\n", strerror(errno));
        munmap(src_buf, TEST_SIZE);
        return 1;
    }

    printf("  ✓ Source allocated:      Virtual %p (%d bytes)\n", (void *)src_buf, TEST_SIZE);
    printf("  ✓ Destination allocated: Virtual %p (%d bytes)\n\n", (void *)dst_buf, TEST_SIZE);

    // Step 5: Initialize buffers
    printf("[4/6] Initializing buffers...\n");
    for (i = 0; i < TEST_SIZE; i++) {
        src_buf[i] = (uint8_t)(i & 0xFF);  // Pattern: 0x00, 0x01, ..., 0xFF, 0x00, ...
    }
    memset(dst_buf, 0xFF, TEST_SIZE);  // Fill with 0xFF (different from source)
    printf("  ✓ Source filled with pattern (0x00, 0x01, ..., 0xFF, ...)\n");
    printf("  ✓ Destination zeroed (0xFF pattern)\n");

    // CRITICAL: Flush destination buffer to prevent CPU cache writeback from
    // overwriting NMP's writes later. The memset() above created dirty cache lines.
    printf("  Flushing destination buffer to DRAM (prevent writeback conflicts)...\n");
    for (i = 0; i < TEST_SIZE; i += 64) {
        asm volatile("clflush (%0)" :: "r"(dst_buf + i) : "memory");
    }
    asm volatile("mfence" ::: "memory");
    printf("  ✓ Destination buffer flushed to DRAM\n\n");

    // Step 6: Translate virtual addresses to physical using pagemap
    printf("[5/6] Translating virtual→physical addresses via /proc/self/pagemap...\n");

    src_phys = virt_to_phys(src_buf);
    if (src_phys == 0) {
        fprintf(stderr, "ERROR: Failed to translate source virtual address %p\n", (void *)src_buf);
        munmap(dst_buf, TEST_SIZE);
        munmap(src_buf, TEST_SIZE);
        return 1;
    }

    dst_phys = virt_to_phys(dst_buf);
    if (dst_phys == 0) {
        fprintf(stderr, "ERROR: Failed to translate destination virtual address %p\n", (void *)dst_buf);
        munmap(dst_buf, TEST_SIZE);
        munmap(src_buf, TEST_SIZE);
        return 1;
    }

    printf("  ✓ Source:      Virtual %p → Physical 0x%llx\n",
           (void *)src_buf, (unsigned long long)src_phys);
    printf("  ✓ Destination: Virtual %p → Physical 0x%llx\n\n",
           (void *)dst_buf, (unsigned long long)dst_phys);

    // Step 7: Program NMP device registers with PHYSICAL addresses
    printf("[6/6] Programming NMP device registers...\n");
    printf("  CRITICAL: Writing PHYSICAL addresses (not virtual)\n");
    printf("    NMP device operates on physical cxl_mem_bus (no MMU)\n\n");

    write_reg_io(bar2_ioport, REG_INPUT_ADDR, src_phys);   // Physical address!
    write_reg_io(bar2_ioport, REG_OUTPUT_ADDR, dst_phys);  // Physical address!
    write_reg_io(bar2_ioport, REG_DATA_SIZE, TEST_SIZE);
    write_reg_io(bar2_ioport, REG_OPCODE, OP_MEMCPY);

    printf("  INPUT_ADDR  = 0x%llx (physical)\n", (unsigned long long)read_reg_io(bar2_ioport, REG_INPUT_ADDR));
    printf("  OUTPUT_ADDR = 0x%llx (physical)\n", (unsigned long long)read_reg_io(bar2_ioport, REG_OUTPUT_ADDR));
    printf("  DATA_SIZE   = %llu bytes\n", (unsigned long long)read_reg_io(bar2_ioport, REG_DATA_SIZE));
    printf("  OPCODE      = %llu (OP_MEMCPY)\n", (unsigned long long)read_reg_io(bar2_ioport, REG_OPCODE));

    // Check initial status
    status = read_reg_io(bar2_ioport, REG_STATUS);
    printf("  Initial STATUS = 0x%llx ", (unsigned long long)status);
    if (status == STATUS_DONE) {
        printf("(DONE - device ready)\n\n");
    } else {
        printf("(Unexpected! Should be DONE)\n\n");
    }

    // Step 7: Start operation and poll status
    printf("Starting NMP operation...\n");

    // CRITICAL: Flush source buffer from CPU caches to DRAM
    // The NMP device reads directly from DRAM via cxl_mem_bus (bypasses CPU caches)
    // Without this flush, NMP would read stale/zero data instead of our test pattern
    printf("  Flushing source buffer from CPU caches to DRAM...\n");
    for (i = 0; i < TEST_SIZE; i += 64) {
        asm volatile("clflush (%0)" :: "r"(src_buf + i) : "memory");
    }
    asm volatile("mfence" ::: "memory");
    printf("  ✓ Source buffer flushed to DRAM\n\n");

    printf("  Writing CONTROL = 0x1 (START)\n");

    // Enable gem5 stats for DMA operation
    m5_reset_stats(0, 0);

    write_reg_io(bar2_ioport, REG_CONTROL, CTRL_START);

    printf("  Polling STATUS register...\n");
    while (1) {
        status = read_reg_io(bar2_ioport, REG_STATUS);
        poll_count++;

        if (status == STATUS_DONE) {
            printf("  ✓ STATUS = DONE after %d polls\n\n", poll_count);
            break;
        } else if (status == STATUS_ERROR) {
            fprintf(stderr, "  ✗ STATUS = ERROR!\n\n");
            m5_dump_stats(0, 0);
            munmap(dst_buf, TEST_SIZE);
            munmap(src_buf, TEST_SIZE);
            return 1;
        } else if (status == STATUS_BUSY) {
            // Still busy, continue polling
            if (poll_count % 1000 == 0) {
                printf("  [STATUS = BUSY, poll #%d]\n", poll_count);
            }
        } else {
            fprintf(stderr, "  ✗ STATUS = 0x%llx (Unknown!)\n\n", (unsigned long long)status);
            m5_dump_stats(0, 0);
            munmap(dst_buf, TEST_SIZE);
            munmap(src_buf, TEST_SIZE);
            return 1;
        }

        // Timeout after 1 million polls
        if (poll_count > 1000000) {
            fprintf(stderr, "  ✗ TIMEOUT: Device did not complete after %d polls\n\n", poll_count);
            m5_dump_stats(0, 0);
            munmap(dst_buf, TEST_SIZE);
            munmap(src_buf, TEST_SIZE);
            return 1;
        }
    }

    m5_dump_stats(0, 0);

    // CRITICAL: Invalidate destination buffer in CPU caches
    // The NMP device wrote directly to DRAM via cxl_mem_bus (bypasses CPU caches)
    // Without this flush, CPU would read stale cached data instead of fresh NMP results
    printf("  Flushing destination buffer from CPU caches...\n");
    for (i = 0; i < TEST_SIZE; i += 64) {
        asm volatile("clflush (%0)" :: "r"(dst_buf + i) : "memory");
    }
    asm volatile("mfence" ::: "memory");
    printf("  ✓ Destination buffer invalidated in caches\n\n");

    // Step 9: Verify result
    printf("[9/9] Verifying result...\n");
    for (i = 0; i < TEST_SIZE; i++) {
        if (src_buf[i] != dst_buf[i]) {
            if (errors < 10) {  // Only print first 10 errors
                fprintf(stderr, "  ERROR at offset %d: src=0x%02x dst=0x%02x\n",
                        i, src_buf[i], dst_buf[i]);
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("  ✓ Verification PASSED - All %d bytes match!\n", TEST_SIZE);
        printf("\n");
        printf("==========================================================\n");
        printf("  ✓✓✓ TEST PASSED ✓✓✓\n");
        printf("  NMP device successfully copied %d bytes via DMA\n", TEST_SIZE);
        printf("  Physical 0x%llx → Physical 0x%llx\n",
               (unsigned long long)src_phys, (unsigned long long)dst_phys);
        printf("  Path: mem_port → cxl_mem_bus → CXL DRAM (bypassed CXL Bridge!)\n");
        printf("==========================================================\n");
    } else {
        fprintf(stderr, "  ✗ Verification FAILED - %d bytes differ\n", errors);
        fprintf(stderr, "\n");
        fprintf(stderr, "==========================================================\n");
        fprintf(stderr, "  ✗✗✗ TEST FAILED ✗✗✗\n");
        fprintf(stderr, "  %d/%d bytes do not match\n", errors, TEST_SIZE);
        fprintf(stderr, "==========================================================\n");
    }

    // Cleanup
    munmap(dst_buf, TEST_SIZE);
    munmap(src_buf, TEST_SIZE);

    return (errors == 0) ? 0 : 1;
}
