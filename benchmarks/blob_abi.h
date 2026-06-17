/*
 * blob_abi.h — entry contract for an OP_EXEC_BLOB code blob.
 *
 * Shared by the blob source (blob_fnv.c) and the device consumer
 * (device_offload.c). This is the ABI between the shipped position-
 * independent code and the device that loads + calls it. It is NOT part
 * of the mailbox layout — struct cxl_mailbox in cxl_mailbox.h is untouched;
 * OP_EXEC_BLOB reuses the existing mailbox fields:
 *
 *   data_off / data_len  -> blob bytes (location/length in the shared region)
 *   arg0                 -> operand-region offset from MB_BASE
 *   arg1                 -> operand count (number of u64s)
 *   arg2                 -> per-run nonce (mixed into the result; see below)
 *
 * The blob is a single freestanding function at offset 0 of the blob bytes:
 *
 *   uint64_t entry(void *args);   // SysV ABI: args in %rdi, return in %rax
 *
 * `args` points at a `struct blob_args` the DEVICE builds in its own local
 * memory (operands are copied out of the UC CXL region into local RAM first,
 * so the blob only ever touches cacheable local memory while it runs). The
 * device casts the loaded bytes to blob_entry_fn and calls it; the return
 * value becomes the mailbox result.
 */

#ifndef BLOB_ABI_H
#define BLOB_ABI_H

#include <stdint.h>

/* Argument block passed to the blob's entry() (built in device-local RAM). */
struct blob_args {
    uint64_t *data;   /* pointer to a local copy of the operand u64 array */
    uint64_t  n;      /* number of u64 operands                           */
    uint64_t  nonce;  /* per-run value the blob mixes into its result     */
};

/* The blob's entry point, as the device calls it. */
typedef uint64_t (*blob_entry_fn)(void *args);

#endif /* BLOB_ABI_H */
