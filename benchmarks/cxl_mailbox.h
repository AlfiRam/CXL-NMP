/*
 * cxl_mailbox.h — shared CXL-DRAM mailbox contract for the MVP
 * host->device code-offload dispatch channel.
 *
 * Both host_offload.c (producer) and device_offload.c (consumer) include
 * this header. The mailbox lives at a fixed PHYSICAL address inside the
 * shared CXL window and is reached from each guest via mmap(/dev/mem)
 * (NOT dd/read/write — the syscall path is capped at top-of-RAM; only
 * mmap reaches above-RAM physical addresses). See the f2 config and
 * benchmarks/device_cxl_test.c for the proven mmap pattern.
 *
 * Physical placement:
 *   - The CXL window is 0x1_0000_0000 .. 0x3_0000_0000 (8 GiB) for both
 *     Systems (host reaches it via CXLBridge/CXLMemory, device via
 *     device_iobridge; both land on the same cxl_dram backing).
 *   - We use the TOP 16 MiB, [0x2ff00_0000, 0x3000_0000_0)... i.e.
 *     MB_BASE = 0x2ff000000, MB_SIZE = 16 MiB.
 *   - The DEVICE already sees the whole CXL window as Reserved E820, so
 *     /dev/mem mmap works there. The HOST enrolls CXL as type-1 RAM, so
 *     the f2 config passes `memmap=16M$0x2ff000000` to the host kernel to
 *     carve this 16 MiB out as Reserved: that (a) keeps the host page
 *     allocator out of it (no clobber) and (b) makes it /dev/mem-mmappable
 *     under STRICT_DEVMEM. mmap of a Reserved region is uncacheable (UC)
 *     on x86, which is what makes the channel ATOMIC-safe (UC stores hit
 *     the shared backing directly; no reliance on clflush under ATOMIC).
 *
 * Forward-compatibility: `data_off`/`data_len` point at a larger shared
 * data region after the header page. Today it carries operand arrays
 * (OP_SUM_ARRAY). A future OP_EXEC_BLOB ships a code blob there and the
 * device maps [data_off, data_len) PROT_EXEC and calls it — an extension,
 * not a redesign.
 */

#ifndef CXL_MAILBOX_H
#define CXL_MAILBOX_H

#include <stdint.h>

/* ---- Physical placement (must match memmap= in x86-cxl-f2-test.py) ---- */
#define MB_BASE      0x2ff000000ULL          /* top 16 MiB of the CXL window */
#define MB_SIZE      (16ULL * 1024 * 1024)   /* mmap window length          */
#define MB_DATA_OFF  0x1000ULL               /* data region starts after the
                                              * 4 KiB header page            */

/* Arbitrary sanity value ("CXL OFFLOAD" in leet) the consumer checks so a
 * mis-mapped / zeroed region is caught instead of silently summing zeros. */
#define MB_MAGIC     0xC7110FF10ADULL

/* layout version — bump on any field change so a stale binary is caught. */
#define MB_VERSION   1u

/* ---- Command / doorbell values (written LAST by the host) ---- */
enum mb_command {
    OP_NONE        = 0,   /* idle / disarmed (consumer writes this to ack) */
    OP_SUM_SCALARS = 1,   /* result = arg0 + arg1                          */
    OP_SUM_ARRAY   = 2,   /* result = sum of (data_len/8) u64s at data_off */
    /* OP_EXEC_BLOB = 3   reserved: map [data_off,data_len) PROT_EXEC, call */
};

/* ---- Status values (written by the device) ---- */
enum mb_status {
    STATUS_IDLE  = 0,
    STATUS_BUSY  = 1,
    STATUS_DONE  = 2,
    STATUS_ERROR = 3,
};

/*
 * Mailbox header. Field order is chosen so every field is naturally
 * aligned with NO padding — the layout is therefore identical for both
 * programs (same gcc, same -O2 -static, same x86-64 ABI) without needing
 * __attribute__((packed)) (packed would force unaligned UC accesses,
 * which we explicitly avoid). 64-bit fields first, then 32-bit fields,
 * then the reserved tail. Total = 144 bytes, well within the 4 KiB header
 * page. Always accessed through a `volatile struct cxl_mailbox *`.
 */
struct cxl_mailbox {
    uint64_t magic;       /* off  0: MB_MAGIC sanity                       */
    uint64_t arg0;        /* off  8: scalar arg 0 (OP_SUM_SCALARS)         */
    uint64_t arg1;        /* off 16: scalar arg 1                          */
    uint64_t arg2;        /* off 24: spare scalar args                     */
    uint64_t arg3;        /* off 32                                        */
    uint64_t data_off;    /* off 40: operand region offset from MB_BASE    */
    uint64_t data_len;    /* off 48: operand region length in bytes        */
    uint64_t result;      /* off 56: device-written result                 */
    uint32_t version;     /* off 64: MB_VERSION                            */
    uint32_t command;     /* off 68: doorbell — see enum mb_command        */
    uint32_t status;      /* off 72: see enum mb_status                    */
    uint32_t opcount;     /* off 76: bumped per completed op (reuse proof) */
    uint64_t reserved[8]; /* off 80: pad / future fields (ends at 144)     */
};

/* ---- Ordering / coherence primitives (the two Systems are NOT cache
 * coherent — they share only the DRAM backing). With the UC mapping these
 * are mostly belt-and-suspenders, but mfence enforces payload-before-flag
 * ordering and clflush is defensive insurance if a mapping ever ends up
 * write-back. ---- */
static inline void mb_mfence(void)
{
    __asm__ __volatile__("mfence" ::: "memory");
}

static inline void mb_clflush(volatile void *p)
{
    __asm__ __volatile__("clflush (%0)" :: "r"(p) : "memory");
}

/* Flush + fence a byte range so a producer's payload is pushed to the
 * shared backing before it rings the doorbell / raises the done flag. */
static inline void mb_flush_range(volatile void *base, uint64_t len)
{
    volatile char *p = (volatile char *)base;
    volatile char *end = p + len;
    for (; p < end; p += 64)
        mb_clflush(p);
    mb_mfence();
}

#endif /* CXL_MAILBOX_H */
