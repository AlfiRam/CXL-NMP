/**
 * CXL NMP Device Pointer Chase Test - Phase 3A
 *
 * Tests the CXLNMPDevice pointer chasing operation (OP_PTR_CHASE).
 *
 * What this does:
 * 1. Allocates array of 100 nodes (64 bytes each) in CXL memory
 * 2. Each node contains: [next_ptr (8B) | data (56B)]
 * 3. Links them sequentially: node[0]->node[1]->...->node[99]->NULL
 * 4. Programs NMP device to chase 10 hops starting from node[0]
 * 5. Verifies NMP device reached node[10]
 *
 * CRITICAL: Linked list stores PHYSICAL addresses (not virtual)
 * - NMP device operates on physical cxl_mem_bus
 * - We use /proc/self/pagemap to translate virtual -> physical
 * - Each node's next pointer is the PHYSICAL address of next node
 *
 * Expected result: NMP device follows 10 pointers and writes final
 * address (physical address of node[10]) to result buffer
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

// NMP Device Register Offsets
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
#define OP_PTR_CHASE     0x1
#define CTRL_START       0x1
#define STATUS_BUSY      0x0
#define STATUS_DONE      0x1
#define STATUS_ERROR     0x2

// Test configuration
#define NUM_NODES        100
#define NODE_SIZE        64
#define PTR_OFFSET       0   // Pointer at start of each node
#define NUM_HOPS         10

// PCI device location
#define NMP_PCI_DEVICE   "0000:00:07.0"

// Page size for virtual→physical translation
#define PAGE_SIZE        4096
#define PAGEMAP_ENTRY_SIZE 8
#define PFN_MASK         ((1ULL << 55) - 1)
#define PAGE_PRESENT     (1ULL << 63)

/**
 * Node structure: 64 bytes per node
 * - Offset 0: next pointer (physical address, 8 bytes)
 * - Offset 8: data/padding (56 bytes)
 */
struct Node {
    uint64_t next;      // Physical address of next node
    uint8_t data[56];   // Padding/data
} __attribute__((packed));

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
 * Translate virtual address to physical address using /proc/self/pagemap
 * Returns 0 on error
 */
uint64_t virt_to_phys(void *virt_addr)
{
    int fd;
    uint64_t page_offset, pfn, phys_addr;
    uint64_t pagemap_entry;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open /proc/self/pagemap: %s\n", strerror(errno));
        return 0;
    }

    page_offset = ((uint64_t)virt_addr / PAGE_SIZE) * PAGEMAP_ENTRY_SIZE;

    if (lseek(fd, page_offset, SEEK_SET) != (off_t)page_offset) {
        fprintf(stderr, "ERROR: Cannot seek in pagemap: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    if (read(fd, &pagemap_entry, PAGEMAP_ENTRY_SIZE) != PAGEMAP_ENTRY_SIZE) {
        fprintf(stderr, "ERROR: Cannot read pagemap entry: %s\n", strerror(errno));
        close(fd);
        return 0;
    }

    close(fd);

    if (!(pagemap_entry & PAGE_PRESENT)) {
        fprintf(stderr, "ERROR: Page not present for virtual address %p\n", virt_addr);
        return 0;
    }

    pfn = pagemap_entry & PFN_MASK;
    phys_addr = (pfn * PAGE_SIZE) + ((uint64_t)virt_addr % PAGE_SIZE);

    return phys_addr;
}

/**
 * Write a 64-bit register via I/O ports
 */
static inline void write_reg_io(uint16_t iobase, int offset, uint64_t value)
{
    uint16_t port = iobase + offset;
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)((value >> 32) & 0xFFFFFFFF);

    outl(low, port);
    outl(high, port + 4);
}

/**
 * Read a 64-bit register via I/O ports
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
    uint16_t bar2_ioport = 0x400;  // Hardcoded I/O port address
    struct Node *nodes;            // Virtual address for CPU access
    uint64_t nodes_phys;           // Physical address of node array
    uint64_t *result_buf;          // Result buffer (virtual)
    uint64_t result_buf_phys;      // Result buffer (physical)
    uint64_t final_addr_expected, final_addr_actual;
    uint64_t status;
    int i;

    printf("==========================================================\n");
    printf("  CXL NMP Device Test - Phase 3A Pointer Chase\n");
    printf("==========================================================\n\n");

    // Step 0: Enable PCI I/O Space
    printf("[0/7] Enabling PCI I/O Space for NMP device...\n");
    if (enable_pci_io_space() != 0) {
        fprintf(stderr, "FAILED: Cannot enable PCI I/O Space\n");
        fprintf(stderr, "  The device BAR will remain [disabled] and inaccessible\n");
        return 1;
    }
    printf("\n");

    // Step 1: Get I/O port access privileges
    printf("[1/7] Requesting I/O port access privileges...\n");
    if (iopl(3) != 0) {
        fprintf(stderr, "ERROR: Cannot get I/O port access: %s\n", strerror(errno));
        fprintf(stderr, "  Try running as root\n");
        return 1;
    }
    printf("  ✓ I/O port access granted (iopl=3)\n\n");

    // Step 2: Allocate node array in CXL memory
    printf("[2/7] Allocating %d nodes (%d bytes each) in CXL memory...\n",
           NUM_NODES, NODE_SIZE);
    printf("  Using MAP_ANONYMOUS (allocated via numactl --membind=1)\n");

    nodes = (struct Node *)mmap(NULL,
                                 NUM_NODES * NODE_SIZE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1,
                                 0);
    if (nodes == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate node array: %s\n", strerror(errno));
        return 1;
    }

    result_buf = (uint64_t *)mmap(NULL,
                                   sizeof(uint64_t),
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS,
                                   -1,
                                   0);
    if (result_buf == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate result buffer: %s\n", strerror(errno));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 1;
    }

    printf("  ✓ Nodes allocated:  Virtual %p (%d bytes)\n",
           (void *)nodes, NUM_NODES * NODE_SIZE);
    printf("  ✓ Result allocated: Virtual %p (8 bytes)\n\n",
           (void *)result_buf);

    // Step 3: Touch all pages to force physical page allocation (demand paging)
    printf("[3/7] Touching pages to trigger physical allocation...\n");
    printf("  MAP_ANONYMOUS uses demand paging - pages allocated on first write\n");

    // Touch all node pages by writing dummy data
    memset(nodes, 0, NUM_NODES * NODE_SIZE);

    // Touch result buffer page
    *result_buf = 0xDEADBEEF;

    printf("  ✓ All pages touched and allocated\n\n");

    // Step 4: Translate base address to physical
    printf("[4/7] Translating node array address via /proc/self/pagemap...\n");
    nodes_phys = virt_to_phys(nodes);
    if (nodes_phys == 0) {
        fprintf(stderr, "ERROR: Failed to translate node array address\n");
        munmap(result_buf, sizeof(uint64_t));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 1;
    }

    result_buf_phys = virt_to_phys(result_buf);
    if (result_buf_phys == 0) {
        fprintf(stderr, "ERROR: Failed to translate result buffer address\n");
        munmap(result_buf, sizeof(uint64_t));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 1;
    }

    printf("  ✓ Nodes:  Virtual %p → Physical 0x%llx\n",
           (void *)nodes, (unsigned long long)nodes_phys);
    printf("  ✓ Result: Virtual %p → Physical 0x%llx\n\n",
           (void *)result_buf, (unsigned long long)result_buf_phys);

    // Step 5: Build linked list (link nodes with PHYSICAL addresses)
    printf("[5/7] Building linked list (nodes[0] -> nodes[1] -> ... -> nodes[99])...\n");
    for (i = 0; i < NUM_NODES - 1; i++) {
        // Each node's next pointer = physical address of next node
        nodes[i].next = nodes_phys + (i + 1) * NODE_SIZE;
        // Fill data with pattern
        memset(nodes[i].data, i & 0xFF, 56);
    }
    // Last node points to NULL (end of list)
    nodes[NUM_NODES - 1].next = 0;
    memset(nodes[NUM_NODES - 1].data, (NUM_NODES - 1) & 0xFF, 56);

    printf("  ✓ Linked list built:\n");
    printf("    node[0]  @ 0x%llx → next = 0x%llx (node[1])\n",
           (unsigned long long)nodes_phys,
           (unsigned long long)nodes[0].next);
    printf("    node[1]  @ 0x%llx → next = 0x%llx (node[2])\n",
           (unsigned long long)(nodes_phys + NODE_SIZE),
           (unsigned long long)nodes[1].next);
    printf("    ...\n");
    printf("    node[99] @ 0x%llx → next = 0x%llx (NULL)\n\n",
           (unsigned long long)(nodes_phys + (NUM_NODES - 1) * NODE_SIZE),
           (unsigned long long)nodes[NUM_NODES - 1].next);

    // Step 6: Flush all buffers to DRAM (cache coherence)
    printf("[6/7] Flushing buffers to DRAM (cache coherence)...\n");
    for (i = 0; i < NUM_NODES * NODE_SIZE; i += 64) {
        asm volatile("clflush (%0)" :: "r"((uint8_t*)nodes + i) : "memory");
    }
    asm volatile("clflush (%0)" :: "r"(result_buf) : "memory");
    asm volatile("mfence" ::: "memory");
    printf("  ✓ All buffers flushed to DRAM\n\n");

    // Step 7: Program NMP device for pointer chase
    printf("[7/7] Programming NMP device for pointer chase...\n");
    printf("  INPUT_ADDR  = 0x%llx (physical address of node[0])\n",
           (unsigned long long)nodes_phys);
    printf("  OUTPUT_ADDR = 0x%llx (where to write final address)\n",
           (unsigned long long)result_buf_phys);
    printf("  DATA_SIZE   = %d (number of hops to perform)\n", NUM_HOPS);
    printf("  RESERVED0   = %d (offset of pointer within each node)\n", PTR_OFFSET);
    printf("  OPCODE      = %d (OP_PTR_CHASE)\n\n", OP_PTR_CHASE);

    write_reg_io(bar2_ioport, REG_INPUT_ADDR, nodes_phys);
    write_reg_io(bar2_ioport, REG_OUTPUT_ADDR, result_buf_phys);
    write_reg_io(bar2_ioport, REG_DATA_SIZE, NUM_HOPS);
    write_reg_io(bar2_ioport, REG_RESERVED0, PTR_OFFSET);
    write_reg_io(bar2_ioport, REG_OPCODE, OP_PTR_CHASE);

    // Check initial status
    status = read_reg_io(bar2_ioport, REG_STATUS);
    if (status != STATUS_DONE) {
        fprintf(stderr, "ERROR: Device not ready (STATUS = 0x%llx)\n",
                (unsigned long long)status);
        munmap(result_buf, sizeof(uint64_t));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 1;
    }

    // Start pointer chase
    printf("Starting pointer chase operation...\n");
    printf("  Expected path: node[0] -> node[1] -> ... -> node[9] -> node[10]\n");

    m5_reset_stats(0, 0);
    write_reg_io(bar2_ioport, REG_CONTROL, CTRL_START);

    // Poll for completion
    printf("  Polling STATUS register...\n");
    int poll_count = 0;
    while (1) {
        status = read_reg_io(bar2_ioport, REG_STATUS);
        poll_count++;

        if (status == STATUS_DONE) {
            printf("  ✓ STATUS = DONE after %d polls\n\n", poll_count);
            break;
        } else if (status == STATUS_ERROR) {
            fprintf(stderr, "  ✗ STATUS = ERROR!\n\n");
            m5_dump_stats(0, 0);
            munmap(result_buf, sizeof(uint64_t));
            munmap(nodes, NUM_NODES * NODE_SIZE);
            return 1;
        }

        if (poll_count > 1000000) {
            fprintf(stderr, "  ✗ TIMEOUT: Device did not complete\n\n");
            m5_dump_stats(0, 0);
            munmap(result_buf, sizeof(uint64_t));
            munmap(nodes, NUM_NODES * NODE_SIZE);
            return 1;
        }
    }

    m5_dump_stats(0, 0);

    // Flush result buffer from cache
    printf("Flushing result buffer from CPU caches...\n");
    asm volatile("clflush (%0)" :: "r"(result_buf) : "memory");
    asm volatile("mfence" ::: "memory");
    printf("  ✓ Result buffer flushed\n\n");

    // Verify result
    printf("[VERIFICATION] Checking result...\n");
    final_addr_expected = nodes_phys + NUM_HOPS * NODE_SIZE;  // node[10]
    final_addr_actual = *result_buf;

    printf("  Expected final address: 0x%llx (node[%d])\n",
           (unsigned long long)final_addr_expected, NUM_HOPS);
    printf("  Actual final address:   0x%llx\n",
           (unsigned long long)final_addr_actual);

    if (final_addr_actual == final_addr_expected) {
        printf("  ✓ Address matches!\n\n");
        printf("==========================================================\n");
        printf("  ✓✓✓ TEST PASSED ✓✓✓\n");
        printf("  NMP device successfully chased %d pointers\n", NUM_HOPS);
        printf("  Started at node[0] (0x%llx)\n",
               (unsigned long long)nodes_phys);
        printf("  Ended at node[%d] (0x%llx)\n",
               NUM_HOPS, (unsigned long long)final_addr_expected);
        printf("  Path: mem_port → cxl_mem_bus → CXL DRAM\n");
        printf("==========================================================\n");

        munmap(result_buf, sizeof(uint64_t));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 0;
    } else {
        printf("  ✗ Address mismatch!\n\n");
        printf("==========================================================\n");
        printf("  ✗✗✗ TEST FAILED ✗✗✗\n");
        printf("  Expected: 0x%llx\n", (unsigned long long)final_addr_expected);
        printf("  Got:      0x%llx\n", (unsigned long long)final_addr_actual);
        printf("==========================================================\n");

        munmap(result_buf, sizeof(uint64_t));
        munmap(nodes, NUM_NODES * NODE_SIZE);
        return 1;
    }
}
