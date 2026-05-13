/*
 * Userspace verification that the device CPU reaches CXL DRAM
 * through cxl_mem_bus.
 *
 * Approach:
 *   - mmap /dev/mem over the CXL range (Reserved in E820 by
 *     DeviceX86Board's _setup_io_devices when cxl_mem_range is given;
 *     STRICT_DEVMEM permits userspace mmap of non-RAM regions).
 *   - Write 0xFEEDFACE_DEADBEEF at offset 0x10000 into the CXL window.
 *   - clflush + mfence to evict the write from the device CPU's L1/L2
 *     so the read-back actually traverses cxl_mem_bus, not just the
 *     local cache.
 *   - Read back; verify.
 *
 * PASS marker: prints "CXL DRAM access TEST PASSED" on success.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Must match configs/example/gem5_library/device-x86-board-cxl-test.py */
#define CXL_BASE      0x100000000ULL
#define CXL_SIZE      (512ULL * 1024 * 1024)  /* 512 MiB */
#define TEST_OFFSET   0x10000ULL              /* 64 KiB into CXL */
#define TEST_PATTERN  0xFEEDFACEDEADBEEFULL

int main(void)
{
    printf("==========================================================\n");
    printf("  device CPU -> cxl_mem_bus -> CXL DRAM\n");
    printf("==========================================================\n");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open /dev/mem: %s\n", strerror(errno));
        fprintf(stderr, "  STRICT_DEVMEM may be blocking. "
                "Verify the CXL range is Reserved in E820 (dmesg).\n");
        return 1;
    }

    /* Map a small window over the CXL range starting at offset 0. */
    size_t map_size = 1ULL << 20;  /* 1 MiB window covers TEST_OFFSET */
    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)CXL_BASE);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap /dev/mem @ 0x%llx (size 0x%lx): %s\n",
                (unsigned long long)CXL_BASE,
                (unsigned long)map_size, strerror(errno));
        close(fd);
        return 1;
    }
    printf("[1/4] mmap /dev/mem @ 0x%llx (size 0x%lx) -> %p\n",
           (unsigned long long)CXL_BASE,
           (unsigned long)map_size, map);

    volatile uint64_t *slot = (volatile uint64_t *)((char *)map + TEST_OFFSET);

    /* Step 1: write the pattern. */
    *slot = TEST_PATTERN;
    printf("[2/4] wrote 0x%016llx to CXL phys 0x%llx\n",
           (unsigned long long)TEST_PATTERN,
           (unsigned long long)(CXL_BASE + TEST_OFFSET));

    /* Step 2: flush the write past local caches so the read-back is
       genuinely a cxl_mem_bus round trip and not a cache hit. */
    asm volatile("clflush (%0)" :: "r"(slot) : "memory");
    asm volatile("mfence" ::: "memory");
    printf("[3/4] clflush + mfence — write evicted past L1/L2\n");

    /* Step 3: read back and verify. */
    uint64_t got = *slot;
    printf("[4/4] read back 0x%016llx (expected 0x%016llx)\n",
           (unsigned long long)got,
           (unsigned long long)TEST_PATTERN);

    int pass = (got == TEST_PATTERN);

    munmap(map, map_size);
    close(fd);

    printf("\n==========================================================\n");
    if (pass) {
        printf("  CXL DRAM access TEST PASSED\n");
        printf("  Device CPU loads/stores reach CXL DRAM via cxl_mem_bus.\n");
    } else {
        printf("  CXL DRAM access TEST FAILED\n");
        printf("  expected: 0x%016llx\n",
               (unsigned long long)TEST_PATTERN);
        printf("  got     : 0x%016llx\n", (unsigned long long)got);
    }
    printf("==========================================================\n");
    return pass ? 0 : 1;
}
