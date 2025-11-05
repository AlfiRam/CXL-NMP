/**
 * Memory Latency Microbenchmark for NMP Research (lmbench-style)
 *
 * Based on lmbench's pointer-chasing methodology for accurate memory
 * latency measurement. Uses a circular linked list with configurable
 * stride to defeat hardware prefetchers and measure true memory latency.
 *
 * Configuration (optimized for DRAM latency measurement):
 * - 2GB array with 4KB stride = 524,288 page-aligned accesses
 * - Working set: 4MB of pointers across 2GB address space
 * - 4KB stride (page size) defeats L3 cache spatial locality
 * - Single traversal ensures all cold misses (no cache reuse)
 * - Pointer chasing pattern defeats prefetcher
 *
 * Address-based routing:
 * - Host path (Core 0): 0x100000000 - 0x2FFFFFFFF
 * - NMP path (Core 1):  0x300000000 - 0x4FFFFFFFF
 *
 * Usage:
 *   NMP_CORE=0 taskset -c 0 ./memory_stride_access  # Host baseline
 *   NMP_CORE=1 taskset -c 1 ./memory_stride_access  # NMP direct
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <gem5/m5ops.h>

// Configuration optimized for DRAM access measurement
// 2GB array with 4KB stride = 524,288 page-aligned accesses
// Working set: 524K elements × 8 bytes = 4MB of pointers
// Address space: 524K pages × 4KB = 2GB (way larger than 96MB L3)
#define SIZE (2ULL * 1024 * 1024 * 1024)  // 2GB
#define STRIDE 4096  // 4KB (page size) - defeats cache, forces DRAM access

// Number of pointer chases per iteration
// lmbench uses 100, we'll use the same
#define HUNDRED 100

// Address ranges for host and NMP paths
#define HOST_BASE_ADDR 0x100000000ULL  // Host CXL path
#define NMP_BASE_ADDR 0x300000000ULL  // NMP direct path

/**
 * Initialize memory with lmbench-style pointer chasing pattern
 * Creates a circular linked list where each pointer is separated by STRIDE
 */
void stride_initialize(char *base, size_t len, size_t stride)
{
    size_t i;

    // Build circular linked list with stride
    for (i = stride; i < len; i += stride) {
        *(char **)&base[i - stride] = (char *)&base[i];
    }
    // Last element loops back to start
    *(char **)&base[i - stride] = (char *)&base[0];
}

int main()
{
    // Determine which core we're running on via environment variable
    char *nmp_core_env = getenv("NMP_CORE");
    int is_nmp_core = (nmp_core_env != NULL && atoi(nmp_core_env) == 1);

    // Select base address based on core
    uint64_t base_addr = is_nmp_core ? NMP_BASE_ADDR : HOST_BASE_ADDR;

    printf("=== Memory Latency Benchmark (lmbench-style) ===\n");
    printf("Core type: %s\n",
           is_nmp_core ? "NMP (Core 1)" : "Host (Core 0)");
    printf("Base address: 0x%llX\n", (unsigned long long)base_addr);
    printf("Size: %llu GB\n",
           (unsigned long long)(SIZE / (1024UL * 1024 * 1024)));
    printf("Stride: %d bytes\n", STRIDE);
    printf("Pattern: Pointer chasing (defeats prefetcher)\n");

    // Verify which CPU we're actually running on
    int actual_cpu = sched_getcpu();
    printf("*** RUNNING ON CPU: %d ***\n", actual_cpu);

    // Check CPU affinity mask for absolute proof
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        printf("CPU Affinity Mask: ");
        for (int i = 0; i < 8; i++) {
            if (CPU_ISSET(i, &mask)) {
                printf("CPU%d ", i);
            }
        }
        printf("\n");

        // Count how many CPUs we're allowed to run on
        int allowed_cpus = 0;
        for (int i = 0; i < 8; i++) {
            if (CPU_ISSET(i, &mask))
                allowed_cpus++;
        }
        if (allowed_cpus == 1) {
            printf("CONFIRMED: Process is pinned to single CPU\n");
        } else {
            printf("WARNING: Process can run on %d CPUs (not pinned!)\n",
                   allowed_cpus);
        }
    }

    if (actual_cpu != (is_nmp_core ? 1 : 0)) {
        printf("WARNING: Expected CPU %d but running on CPU %d!\n",
               is_nmp_core ? 1 : 0, actual_cpu);
    }
    printf("================================\n");

    // Use mmap to allocate at specific address
    char *data =
        (char *)mmap((void *)base_addr,  // Requested address
                     SIZE,  // Size
                     PROT_READ | PROT_WRITE,  // Permissions
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,  // Flags
                     -1,  // No file descriptor
                     0  // No offset
        );

    if (data == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap failed for address 0x%lX\n",
                base_addr);
        return 1;
    }

    printf("Memory mapped at: %p\n", (void *)data);

    // Initialize memory to ensure pages are allocated
    printf("Initializing memory with pointer-chasing pattern...\n");
    memset(data, 0, SIZE);  // Touch all pages first

    // Build pointer-chasing linked list (lmbench style)
    stride_initialize(data, SIZE, STRIDE);

    // Calculate number of iterations
    // Single traversal ensures all accesses are COLD MISSES (no cache reuse)
    // With 4KB stride, each access touches a different page
    size_t chain_length = SIZE / STRIDE;  // Number of elements in chain
    // SINGLE traversal for pure DRAM latency
    size_t iterations = chain_length * 1;

    printf("Chain elements: %zu\n", chain_length);
    printf("Total pointer chases: %zu\n", iterations);
    printf("Starting benchmark...\n");

    // RESET STATS - The absolute last thing before the loop
    m5_reset_stats(0, 0);

    // Pointer chasing loop (lmbench style)
    // This defeats hardware prefetchers because each access depends
    // on the previous one
    register char **p = (char **)data;
    register size_t count = iterations;

    while (count-- > 0) {
        // Each iteration does one pointer dereference
        // The next address can't be known until current load completes
        p = (char **)*p;
    }

    // Dump stats
    m5_dump_stats(0, 0);

    printf("Benchmark complete\n");
    // Use the pointer to prevent optimization
    printf("Final pointer: %p\n", (void *)p);

    // Verify we stayed on the same CPU throughout execution
    int final_cpu = sched_getcpu();
    printf("Final CPU check: %d ", final_cpu);
    if (final_cpu == actual_cpu) {
        printf("(STABLE - stayed on CPU %d)\n", final_cpu);
    } else {
        printf("(MIGRATED - started on CPU %d!)\n", actual_cpu);
    }

    // Unmap memory
    munmap(data, SIZE);
    return 0;
}
