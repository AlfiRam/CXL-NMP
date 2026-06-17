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
#include "blob_abi.h"

/* Poll budget mirrors the host: the device may start before the host has
 * armed the doorbell, so it waits up to POLL_TRIES * POLL_SLEEP_US sim-us. */
#define POLL_TRIES     100000
#define POLL_SLEEP_US  1000

/* OP_EXEC_BLOB sanity caps — bound the shipped code and operand sizes before
 * we copy or execute anything out of the shared region. */
#define MAX_BLOB_LEN   (1u << 20)   /* 1 MiB cap on shipped code */
#define MAX_OPS        (1u << 20)   /* 1Mi-u64 operand cap       */

static uint32_t read_command(volatile struct cxl_mailbox *mb)
{
    mb_clflush(&mb->command);
    mb_mfence();
    return mb->command;
}

static const char *cmd_name(uint32_t cmd)
{
    switch (cmd) {
    case OP_SUM_SCALARS: return "OP_SUM_SCALARS";
    case OP_SUM_ARRAY:   return "OP_SUM_ARRAY";
    case OP_EXEC_BLOB:   return "OP_EXEC_BLOB";
    default:             return "OP_UNKNOWN";
    }
}

/* Report a device-side error per the termination contract (a "DEVICE OFFLOAD
 * ERROR ..." line so the f2 barrier ends the run) and tear down the mailbox
 * mapping. Caller returns 1 afterward. */
static void device_error(volatile struct cxl_mailbox *mb, void *map, int fd,
                         const char *msg)
{
    mb->status = STATUS_ERROR;
    mb->command = OP_NONE;
    mb_flush_range((volatile void *)mb, sizeof(*mb));
    printf("DEVICE OFFLOAD ERROR %s\n", msg);
    munmap(map, MB_SIZE);
    close(fd);
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
    case OP_EXEC_BLOB: {
        /* Ship-a-binary path: the host placed a position-independent code
         * blob at [data_off, data_len) and the operands at arg0. We copy the
         * blob into a LOCAL executable page and call entry(args) on this
         * device core (copy-then-exec — never execute from the UC CXL
         * mapping). arg0=operand offset, arg1=count, arg2=nonce. */
        uint64_t blob_off = mb->data_off;
        uint64_t blob_len = mb->data_len;
        uint64_t ops_off  = mb->arg0;
        uint64_t n        = mb->arg1;
        uint64_t nonce    = mb->arg2;
        printf("[device] op=OP_EXEC_BLOB blob@0x%llx len=%llu "
               "ops@0x%llx n=%llu nonce=0x%llx\n",
               (unsigned long long)blob_off, (unsigned long long)blob_len,
               (unsigned long long)ops_off, (unsigned long long)n,
               (unsigned long long)nonce);

        /* Bounds: keep blob + operands inside the mapped window and within
         * sane caps before copying or executing anything. */
        if (blob_len == 0 || blob_len > MAX_BLOB_LEN ||
            blob_off > MB_SIZE || blob_off + blob_len > MB_SIZE ||
            n > MAX_OPS ||
            ops_off > MB_SIZE || ops_off + n * sizeof(uint64_t) > MB_SIZE) {
            device_error(mb, map, fd, "OP_EXEC_BLOB bounds check failed");
            return 1;
        }

        /* Executable page in device-LOCAL cacheable RAM. Instruction fetch
         * from here is the normal proven path under AtomicSimpleCPU. */
        long pg = sysconf(_SC_PAGESIZE);
        size_t map_len = ((size_t)blob_len + pg - 1) & ~((size_t)pg - 1);
        void *code = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED) {
            device_error(mb, map, fd, "OP_EXEC_BLOB mmap exec page failed");
            return 1;
        }

        /* Copy blob bytes out of the UC CXL region into local RAM. */
        memcpy(code, (char *)map + blob_off, blob_len);

        /* Flip to R+X (canonical JIT W->X) and sync I/D for the freshly
         * written code before we fetch it. */
        if (mprotect(code, map_len, PROT_READ | PROT_EXEC) != 0) {
            munmap(code, map_len);
            device_error(mb, map, fd, "OP_EXEC_BLOB mprotect R+X failed");
            return 1;
        }
        __builtin___clear_cache((char *)code, (char *)code + blob_len);

        /* Copy operands local too, so the blob touches ONLY local cacheable
         * memory while it runs (no UC reads mid-execution). */
        uint64_t *ops = NULL;
        if (n > 0) {
            ops = (uint64_t *)malloc(n * sizeof(uint64_t));
            if (!ops) {
                munmap(code, map_len);
                device_error(mb, map, fd,
                             "OP_EXEC_BLOB malloc operands failed");
                return 1;
            }
            memcpy(ops, (char *)map + ops_off, n * sizeof(uint64_t));
        }

        /* Call the shipped code on this device core. */
        struct blob_args a = { ops, n, nonce };
        blob_entry_fn fn = (blob_entry_fn)code;
        result = fn(&a);
        printf("[device] blob entry() returned %llu (0x%llx)\n",
               (unsigned long long)result, (unsigned long long)result);

        free(ops);
        munmap(code, map_len);
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

    printf("DEVICE OFFLOAD done op=%s (opcount=%u) result=%llu\n",
           cmd_name(cmd), mb->opcount, (unsigned long long)result);

    munmap(map, MB_SIZE);
    close(fd);
    return 0;
}
