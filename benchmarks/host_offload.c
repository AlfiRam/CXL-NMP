/*
 * host_offload.c — host side of the MVP host->device CXL offload channel.
 *
 * Runs on the HOST guest Linux. Maps the shared CXL mailbox via
 * mmap(/dev/mem) (the host kernel must have carved [MB_BASE, MB_BASE+16M)
 * out as Reserved via `memmap=16M$0x2ff000000`, which the f2 config passes
 * under --offload; otherwise the mmap fails loudly here). Writes a task
 * into the mailbox, rings the doorbell, polls for completion, verifies the
 * device's result against a locally computed expected value.
 *
 * Termination contract: EVERY exit path prints a line beginning with
 * "HOST OFFLOAD " (OK / FAIL / TIMEOUT / ERROR) so the f2 serial barrier
 * terminates the run on failure as well as success. The only way the run
 * hangs is if this process never prints that prefix — which then
 * unambiguously means a dead process/sim, not a slow one.
 *
 * Build: gcc -O2 -static -Wall (see benchmarks/Makefile). No m5ops.
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

#include "cxl_mailbox.h"

/* Demo operand count for OP_SUM_ARRAY: 256 u64s (2 KiB, fits the first
 * page of the data region). Expected sum = 1 + 2 + ... + N. */
#define DEMO_N 256

/* Poll budget: POLL_TRIES * POLL_SLEEP_US microseconds of simulated time.
 * usleep advances sim time cheaply (vs. hammering the CXL path with UC
 * reads) while the device, sharing the event queue, makes progress. */
#define POLL_TRIES     100000
#define POLL_SLEEP_US  1000

static uint32_t read_status(volatile struct cxl_mailbox *mb)
{
    /* Defensive under a write-back mapping; a no-op under UC. */
    mb_clflush(&mb->status);
    mb_mfence();
    return mb->status;
}

int main(void)
{
    /* gem5's Terminal is unit-buffered on the host side, but make the
     * guest libc stdout unbuffered too so the marker reaches serial
     * immediately on every path. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== host_offload: CXL mailbox dispatch (producer) ===\n");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("HOST OFFLOAD ERROR open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    void *map = mmap(NULL, MB_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)MB_BASE);
    if (map == MAP_FAILED) {
        printf("HOST OFFLOAD ERROR mmap /dev/mem @ 0x%llx (size 0x%llx): %s\n",
               (unsigned long long)MB_BASE, (unsigned long long)MB_SIZE,
               strerror(errno));
        printf("  (host needs memmap=16M$0x2ff000000 to make this Reserved)\n");
        close(fd);
        return 1;
    }

    volatile struct cxl_mailbox *mb = (volatile struct cxl_mailbox *)map;
    volatile uint64_t *data = (volatile uint64_t *)((char *)map + MB_DATA_OFF);

    printf("[host] mmap /dev/mem @ 0x%llx -> %p\n",
           (unsigned long long)MB_BASE, map);

    /* ---- Build the task ---- */
    uint64_t expected = 0;
    for (int i = 0; i < DEMO_N; i++) {
        data[i] = (uint64_t)(i + 1);
        expected += (uint64_t)(i + 1);
    }

    mb->magic    = MB_MAGIC;
    mb->version  = MB_VERSION;
    mb->data_off = MB_DATA_OFF;
    mb->data_len = (uint64_t)DEMO_N * sizeof(uint64_t);
    mb->result   = 0;
    mb->status   = STATUS_IDLE;
    mb->command  = OP_NONE;   /* disarmed until the doorbell below */

    /* Push payload + header to the shared backing BEFORE arming. */
    mb_flush_range(data, (uint64_t)DEMO_N * sizeof(uint64_t));
    mb_flush_range((volatile void *)mb, sizeof(*mb));

    printf("[host] armed OP_SUM_ARRAY N=%d expected=%llu\n",
           DEMO_N, (unsigned long long)expected);

    /* ---- Ring the doorbell (command written LAST) ---- */
    mb->command = OP_SUM_ARRAY;
    mb_clflush(&mb->command);
    mb_mfence();

    /* ---- Poll for completion ---- */
    uint32_t st = STATUS_IDLE;
    int done = 0;
    for (long t = 0; t < POLL_TRIES; t++) {
        st = read_status(mb);
        if (st == STATUS_DONE || st == STATUS_ERROR) {
            done = 1;
            break;
        }
        usleep(POLL_SLEEP_US);
    }

    if (!done) {
        printf("HOST OFFLOAD TIMEOUT (last status=%u, no device response)\n", st);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    if (st == STATUS_ERROR) {
        printf("HOST OFFLOAD FAIL device reported STATUS_ERROR (opcount=%u)\n",
               mb->opcount);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    /* Fresh read of the result from the backing. */
    mb_clflush(&mb->result);
    mb_mfence();
    uint64_t got = mb->result;

    int ok = (got == expected);
    printf("HOST OFFLOAD result=%llu expected=%llu %s (opcount=%u)\n",
           (unsigned long long)got, (unsigned long long)expected,
           ok ? "OK" : "FAIL", mb->opcount);

    munmap(map, MB_SIZE);
    close(fd);
    return ok ? 0 : 1;
}
