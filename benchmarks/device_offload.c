/*
 * device_offload.c — device side of the MVP host->device CXL offload channel.
 *
 * Runs on the DEVICE guest Linux (the 2-core CXL device System). Maps the
 * shared CXL mailbox via mmap(/dev/mem) — the device already sees the CXL
 * window as Reserved E820, so this works under STRICT_DEVMEM with no extra
 * kernel arg. Polls the command word (the host's doorbell), executes the
 * selected fixed operation on the device cores, writes the result back, and
 * raises STATUS_DONE.
 *
 * Termination contract: EVERY exit path prints a line beginning with
 * "DEVICE OFFLOAD " (done / TIMEOUT / ERROR) so the f2 serial barrier
 * terminates the run on failure as well as success.
 *
 * MVP scope: services exactly ONE transaction, then exits (a loop of N is a
 * trivial extension). The operation is selected by the command word, NOT a
 * shipped binary — but data_off/data_len already point at a larger shared
 * region so OP_EXEC_BLOB (ship + run a code blob) is a later extension.
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

/* Poll budget mirrors the host: the device may start before the host has
 * armed the doorbell, so it waits up to POLL_TRIES * POLL_SLEEP_US sim-us. */
#define POLL_TRIES     100000
#define POLL_SLEEP_US  1000

static uint32_t read_command(volatile struct cxl_mailbox *mb)
{
    mb_clflush(&mb->command);
    mb_mfence();
    return mb->command;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== device_offload: CXL mailbox dispatch (consumer) ===\n");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("DEVICE OFFLOAD ERROR open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    void *map = mmap(NULL, MB_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)MB_BASE);
    if (map == MAP_FAILED) {
        printf("DEVICE OFFLOAD ERROR mmap /dev/mem @ 0x%llx (size 0x%llx): %s\n",
               (unsigned long long)MB_BASE, (unsigned long long)MB_SIZE,
               strerror(errno));
        close(fd);
        return 1;
    }

    volatile struct cxl_mailbox *mb = (volatile struct cxl_mailbox *)map;
    volatile uint64_t *data = (volatile uint64_t *)((char *)map + MB_DATA_OFF);

    printf("[device] mmap /dev/mem @ 0x%llx -> %p\n",
           (unsigned long long)MB_BASE, map);
    printf("[device] waiting for host doorbell...\n");

    /* ---- Wait for the doorbell ---- */
    uint32_t cmd = OP_NONE;
    int rang = 0;
    for (long t = 0; t < POLL_TRIES; t++) {
        cmd = read_command(mb);
        if (cmd != OP_NONE) {
            rang = 1;
            break;
        }
        usleep(POLL_SLEEP_US);
    }

    if (!rang) {
        printf("DEVICE OFFLOAD TIMEOUT (no doorbell from host)\n");
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    /* Doorbell rung — fence in the host's payload, sanity-check the magic. */
    mb_mfence();
    if (mb->magic != MB_MAGIC) {
        mb->status = STATUS_ERROR;
        mb_clflush(&mb->status);
        mb_mfence();
        printf("DEVICE OFFLOAD ERROR bad magic 0x%llx (expected 0x%llx)\n",
               (unsigned long long)mb->magic, (unsigned long long)MB_MAGIC);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    mb->status = STATUS_BUSY;
    mb_clflush(&mb->status);
    mb_mfence();

    /* ---- Execute the selected fixed operation ---- */
    uint64_t result = 0;
    int ok = 1;
    switch (cmd) {
    case OP_SUM_SCALARS:
        result = mb->arg0 + mb->arg1;
        printf("[device] op=OP_SUM_SCALARS %llu + %llu\n",
               (unsigned long long)mb->arg0, (unsigned long long)mb->arg1);
        break;
    case OP_SUM_ARRAY: {
        uint64_t n = mb->data_len / sizeof(uint64_t);
        printf("[device] op=OP_SUM_ARRAY n=%llu @ off 0x%llx\n",
               (unsigned long long)n, (unsigned long long)mb->data_off);
        for (uint64_t i = 0; i < n; i++)
            result += data[i];
        break;
    }
    default:
        ok = 0;
        break;
    }

    if (!ok) {
        mb->status = STATUS_ERROR;
        mb->command = OP_NONE;
        mb_flush_range((volatile void *)mb, sizeof(*mb));
        printf("DEVICE OFFLOAD ERROR unknown command=%u\n", cmd);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    /* ---- Write result, then raise DONE (payload before flag) ---- */
    mb->result = result;
    mb->opcount = mb->opcount + 1;
    mb_clflush(&mb->result);
    mb_clflush(&mb->opcount);
    mb_mfence();

    mb->status = STATUS_DONE;
    mb_clflush(&mb->status);
    mb_mfence();

    mb->command = OP_NONE;   /* ack / disarm the doorbell */
    mb_clflush(&mb->command);
    mb_mfence();

    printf("DEVICE OFFLOAD done op=%u result=%llu\n",
           mb->opcount, (unsigned long long)result);

    munmap(map, MB_SIZE);
    close(fd);
    return 0;
}
