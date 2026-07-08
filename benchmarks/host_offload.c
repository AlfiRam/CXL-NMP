/*
 * host_offload.c — host side of the host->device CXL offload channel.
 *
 * Runs on the HOST guest Linux. Maps the shared CXL mailbox via
 * mmap(/dev/mem) (the host kernel must have carved [MB_BASE, MB_BASE+16M)
 * out as Reserved via `memmap=16M$0x2ff000000`, which the f2 config passes
 * under --offload; otherwise the mmap fails loudly here). Writes a task into
 * the mailbox, rings the doorbell, polls for completion, verifies the
 * device's result against a locally computed expected value.
 *
 * Three modes, selected by argv[1] (default = xtea):
 *   (default)/"xtea" -> OP_EXEC_BLOB shipping the XTEA cipher blob: ship a
 *                       128-bit key + 64-bit plaintext block; the device
 *                       copies the blob into an exec page and CALLS it,
 *                       returning the ciphertext. Expected = the same XTEA
 *                       encryption computed locally => a match proves the
 *                       device executed the shipped key-dependent cipher.
 *   "fnv"            -> OP_EXEC_BLOB shipping the FNV hash blob (the proven
 *                       exec path, kept reachable as a regression).
 *   "sum"            -> OP_SUM_ARRAY (device sums the operand array).
 *   "handoff" <base> <size>
 *                    -> OP_HANDOFF: §6.2 subtree handoff + compute on the
 *                       handed-off data. Ships the XTEA blob in the mailbox
 *                       (control channel) but writes the key/plaintext into
 *                       the handed-off PROTECTED region at base+HANDOFF_OPS_OFF
 *                       (data path). Expected = local XTEA => a match proves
 *                       the device computed on data read out of the protected
 *                       region. Needs a second host memmap= carve of the
 *                       region (the f2 config adds it under --device-handoff).
 *
 * Termination contract: EVERY exit path prints a line beginning with
 * "HOST OFFLOAD " (OK / FAIL / TIMEOUT / ERROR) so the f2 serial barrier
 * terminates the run on failure as well as success.
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
#include "blob_bytes.h"        /* generated: cxl_blob[],      cxl_blob_len      (FNV) */
#include "blob_xtea_bytes.h"   /* generated: cxl_blob_xtea[], cxl_blob_xtea_len (XTEA) */

/* FNV demo: 256 u64 operands. */
#define DEMO_N      256

/* XTEA: 6 operands — key[0..3], v0, v1 — one u32 per u64 slot (low 32 bits). */
#define XTEA_N      6

/* Where operands live in the data region (blob sits at MB_DATA_OFF=0x1000;
 * operands a page later so code and data don't overlap). */
#define OPS_OFF     0x2000ULL

/* FNV per-run nonce (see host_offload FNV path). */
#define HOST_NONCE  0xA5A5A5A5DEADBEEFULL

/* XTEA test vector: a fixed distinctive 128-bit key + 64-bit plaintext. The
 * host ships these and replicates the encryption locally; the device can only
 * match by executing the shipped cipher on this key. */
#define XK0 0x12345678u
#define XK1 0x9ABCDEF0u
#define XK2 0xFEDCBA98u
#define XK3 0x76543210u
#define XP0 0xDEADBEEFu
#define XP1 0xCAFEF00Du

/* Poll budget: POLL_TRIES * POLL_SLEEP_US microseconds of simulated time. */
#define POLL_TRIES     100000
#define POLL_SLEEP_US  1000

static uint32_t read_status(volatile struct cxl_mailbox *mb)
{
    /* Defensive under a write-back mapping; a no-op under UC. */
    mb_clflush(&mb->status);
    mb_mfence();
    return mb->status;
}

/* Host replica of blob_fnv.c (FNV-1a 64-bit, then XOR nonce). */
static uint64_t fnv1a_nonce(const uint64_t *data, uint64_t n, uint64_t nonce)
{
    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a 64-bit offset basis */
    for (uint64_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;            /* FNV-1a 64-bit prime */
    }
    h ^= nonce;
    return h;
}

/* Host replica of blob_xtea.c — MUST match its constants/rounds/types/packing
 * exactly (uint32_t throughout; 32-round XTEA; result = v0|(v1<<32)). */
static uint64_t xtea_encrypt(const uint32_t key[4], uint32_t v0, uint32_t v1)
{
    uint32_t sum = 0;
    const uint32_t delta = 0x9E3779B9u;
    for (int i = 0; i < 32; i++) {
        v0  += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1  += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    return ((uint64_t)v1 << 32) | (uint64_t)v0;
}

int main(int argc, char **argv)
{
    /* gem5's Terminal is unit-buffered on the host side, but make the guest
     * libc stdout unbuffered too so the marker reaches serial immediately. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Mode selection: default to xtea (this milestone's path). */
    const char *mode = (argc > 1) ? argv[1] : "xtea";
    int is_sum = (strcmp(mode, "sum") == 0);
    int is_fnv = (strcmp(mode, "fnv") == 0);
    int is_handoff = (strcmp(mode, "handoff") == 0);
    /* default / "xtea" / anything else -> XTEA (the final else branch below) */

    const char *opname = is_sum ? "OP_SUM_ARRAY"
                       : is_fnv ? "OP_EXEC_BLOB/fnv"
                       : is_handoff ? "OP_HANDOFF"
                                : "OP_EXEC_BLOB/xtea";

    printf("=== host_offload: CXL mailbox dispatch (producer) [%s] ===\n",
           opname);

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
    printf("[host] mmap /dev/mem @ 0x%llx -> %p\n",
           (unsigned long long)MB_BASE, map);

    /* Common header init (command stays OP_NONE until the doorbell). */
    mb->magic   = MB_MAGIC;
    mb->version = MB_VERSION;
    mb->result  = 0;
    mb->status  = STATUS_IDLE;
    mb->command = OP_NONE;

    uint64_t expected;
    uint32_t op;

    if (is_sum) {
        /* Regression: operands at data_off, device sums them. */
        uint64_t *data = (uint64_t *)((char *)map + MB_DATA_OFF);
        uint64_t sum = 0;
        for (int i = 0; i < DEMO_N; i++) {
            data[i] = (uint64_t)(i + 1);
            sum += (uint64_t)(i + 1);
        }
        mb->data_off = MB_DATA_OFF;
        mb->data_len = (uint64_t)DEMO_N * sizeof(uint64_t);
        expected = sum;
        op = OP_SUM_ARRAY;

        mb_flush_range(data, (uint64_t)DEMO_N * sizeof(uint64_t));
        mb_flush_range((volatile void *)mb, sizeof(*mb));
        printf("[host] armed OP_SUM_ARRAY N=%d expected=%llu\n",
               DEMO_N, (unsigned long long)expected);

    } else if (is_fnv) {
        /* Ship the FNV blob at data_off, 256 operands at OPS_OFF. */
        const unsigned char *blob = cxl_blob;
        unsigned int blob_len = cxl_blob_len;
        if ((uint64_t)blob_len > OPS_OFF - MB_DATA_OFF) {
            printf("HOST OFFLOAD ERROR fnv blob too large (%u bytes)\n", blob_len);
            munmap(map, MB_SIZE); close(fd); return 1;
        }
        memcpy((char *)map + MB_DATA_OFF, blob, blob_len);

        uint64_t local_ops[DEMO_N];
        uint64_t *ops = (uint64_t *)((char *)map + OPS_OFF);
        for (int i = 0; i < DEMO_N; i++) {
            local_ops[i] = (uint64_t)(i + 1);
            ops[i] = local_ops[i];
        }
        mb->data_off = MB_DATA_OFF;
        mb->data_len = blob_len;
        mb->arg0 = OPS_OFF;
        mb->arg1 = DEMO_N;
        mb->arg2 = HOST_NONCE;
        expected = fnv1a_nonce(local_ops, DEMO_N, HOST_NONCE);
        op = OP_EXEC_BLOB;

        mb_flush_range((char *)map + MB_DATA_OFF, blob_len);
        mb_flush_range((char *)map + OPS_OFF, (uint64_t)DEMO_N * sizeof(uint64_t));
        mb_flush_range((volatile void *)mb, sizeof(*mb));
        printf("[host] armed OP_EXEC_BLOB/fnv blob_len=%u N=%d nonce=0x%llx "
               "expected=%llu\n", blob_len, DEMO_N,
               (unsigned long long)HOST_NONCE, (unsigned long long)expected);

    } else if (is_handoff) {
        /* §6.2 subtree handoff (range-keyed) + compute on the handed-off data:
         * hand the device AUTHORITY over a contiguous PROTECTED CXL data range
         * AND ship XTEA work whose operands live INSIDE that range. The
         * mailbox stays a pure control channel (descriptor + blob); the
         * key/plaintext go into protected DRAM at region_base+HANDOFF_OPS_OFF
         * via a SECOND /dev/mem mmap (the config carves the region Reserved on
         * the host with a second memmap=). base/size come from argv (the
         * config computes the aligned region inside the device's protected CXL
         * window and passes `host_offload handoff <base> <size>`). */
        uint64_t region_base = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;
        uint64_t region_size = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;
        if (region_base == 0 || region_size < HANDOFF_OPS_OFF +
                XTEA_N * sizeof(uint64_t)) {
            printf("HOST OFFLOAD ERROR handoff region [0x%llx..+0x%llx) "
                   "missing or too small for operands\n",
                   (unsigned long long)region_base,
                   (unsigned long long)region_size);
            munmap(map, MB_SIZE); close(fd); return 1;
        }

        void *rmap = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, (off_t)region_base);
        if (rmap == MAP_FAILED) {
            printf("HOST OFFLOAD ERROR mmap /dev/mem @ 0x%llx (size 0x%llx): "
                   "%s\n", (unsigned long long)region_base,
                   (unsigned long long)region_size, strerror(errno));
            printf("  (host needs a memmap= carve of the handoff region to "
                   "make it Reserved)\n");
            munmap(map, MB_SIZE); close(fd); return 1;
        }

        /* Blob (the binary) stays on the control channel: mailbox data page. */
        const unsigned char *blob = cxl_blob_xtea;
        unsigned int blob_len = cxl_blob_xtea_len;
        if ((uint64_t)blob_len > MB_SIZE - MB_DATA_OFF) {
            printf("HOST OFFLOAD ERROR xtea blob too large (%u bytes)\n",
                   blob_len);
            munmap(rmap, region_size); munmap(map, MB_SIZE); close(fd);
            return 1;
        }
        memcpy((char *)map + MB_DATA_OFF, blob, blob_len);

        /* Operands (the DATA) go into the handed-off protected region, page 1
         * (page 0 reserved) — same six-slot layout as the xtea branch. */
        const uint32_t key[4] = { XK0, XK1, XK2, XK3 };
        uint64_t *ops = (uint64_t *)((char *)rmap + HANDOFF_OPS_OFF);
        ops[0] = (uint64_t)key[0];
        ops[1] = (uint64_t)key[1];
        ops[2] = (uint64_t)key[2];
        ops[3] = (uint64_t)key[3];
        ops[4] = (uint64_t)XP0;
        ops[5] = (uint64_t)XP1;

        mb->data_off = MB_DATA_OFF;
        mb->data_len = blob_len;
        mb->arg0 = region_base;
        mb->arg1 = region_size;
        mb->arg2 = region_base + HANDOFF_OPS_OFF;  /* operand ABSOLUTE addr */
        mb->arg3 = XTEA_N;
        expected = xtea_encrypt(key, XP0, XP1);
        op = OP_HANDOFF;

        mb_flush_range((char *)map + MB_DATA_OFF, blob_len);
        mb_flush_range(ops, (uint64_t)XTEA_N * sizeof(uint64_t));
        mb_flush_range((volatile void *)mb, sizeof(*mb));
        munmap(rmap, region_size);
        printf("[host] armed OP_HANDOFF region=[0x%llx..0x%llx) size=0x%llx "
               "ops@0x%llx n=%d blob_len=%u expected=%llu (0x%llx)\n",
               (unsigned long long)region_base,
               (unsigned long long)(region_base + region_size),
               (unsigned long long)region_size,
               (unsigned long long)(region_base + HANDOFF_OPS_OFF),
               XTEA_N, blob_len,
               (unsigned long long)expected, (unsigned long long)expected);

    } else {
        /* XTEA: ship the cipher blob at data_off, key+block at OPS_OFF.
         * Operand layout (one u32 per u64 slot): data[0..3]=key, data[4]=v0,
         * data[5]=v1. */
        const unsigned char *blob = cxl_blob_xtea;
        unsigned int blob_len = cxl_blob_xtea_len;
        if ((uint64_t)blob_len > OPS_OFF - MB_DATA_OFF) {
            printf("HOST OFFLOAD ERROR xtea blob too large (%u bytes)\n", blob_len);
            munmap(map, MB_SIZE); close(fd); return 1;
        }
        memcpy((char *)map + MB_DATA_OFF, blob, blob_len);

        const uint32_t key[4] = { XK0, XK1, XK2, XK3 };
        uint64_t *ops = (uint64_t *)((char *)map + OPS_OFF);
        ops[0] = (uint64_t)key[0];
        ops[1] = (uint64_t)key[1];
        ops[2] = (uint64_t)key[2];
        ops[3] = (uint64_t)key[3];
        ops[4] = (uint64_t)XP0;
        ops[5] = (uint64_t)XP1;

        mb->data_off = MB_DATA_OFF;
        mb->data_len = blob_len;
        mb->arg0 = OPS_OFF;
        mb->arg1 = XTEA_N;
        mb->arg2 = 0;            /* nonce unused for XTEA */
        expected = xtea_encrypt(key, XP0, XP1);
        op = OP_EXEC_BLOB;

        mb_flush_range((char *)map + MB_DATA_OFF, blob_len);
        mb_flush_range((char *)map + OPS_OFF, (uint64_t)XTEA_N * sizeof(uint64_t));
        mb_flush_range((volatile void *)mb, sizeof(*mb));
        printf("[host] armed OP_EXEC_BLOB/xtea blob_len=%u key=%08x%08x%08x%08x "
               "pt=%08x%08x expected=%llu (0x%llx)\n",
               blob_len, XK0, XK1, XK2, XK3, XP0, XP1,
               (unsigned long long)expected, (unsigned long long)expected);
    }

    /* ---- Ring the doorbell (command written LAST) ---- */
    mb->command = op;
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
        printf("HOST OFFLOAD TIMEOUT op=%s (last status=%u, no device response)\n",
               opname, st);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    if (st == STATUS_ERROR) {
        printf("HOST OFFLOAD FAIL op=%s device reported STATUS_ERROR (opcount=%u)\n",
               opname, mb->opcount);
        munmap(map, MB_SIZE);
        close(fd);
        return 1;
    }

    /* Fresh read of the result from the backing. */
    mb_clflush(&mb->result);
    mb_mfence();
    uint64_t got = mb->result;

    int ok = (got == expected);
    printf("HOST OFFLOAD op=%s result=%llu expected=%llu %s (opcount=%u)\n",
           opname, (unsigned long long)got, (unsigned long long)expected,
           ok ? "OK" : "FAIL", mb->opcount);

    munmap(map, MB_SIZE);
    close(fd);
    return ok ? 0 : 1;
}
