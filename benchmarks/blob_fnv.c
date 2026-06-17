/*
 * blob_fnv.c — the shipped code blob for OP_EXEC_BLOB.
 *
 * This is compiled freestanding and its raw .text is extracted (objcopy)
 * into blob.bin, which the host ships through the CXL mailbox and the device
 * copies into an executable page and calls. See benchmarks/Makefile for the
 * build + the objdump verify gate (entry must be at .text offset 0 with ZERO
 * relocations).
 *
 * DISCIPLINE — what makes the raw .text position-independent and relocation-
 * free *by construction*, so it runs wherever the device places it:
 *   - ONE function (entry) in this translation unit -> it sits at .text
 *     offset 0, which is the fixed entry contract (blob_abi.h).
 *   - NO global variables, NO string/array literals (no .rodata), NO calls
 *     to libc or any external symbol. The function touches only its `args`
 *     pointer, its stack, and immediate constants. Such code uses only
 *     RIP/stack/register-relative addressing -> inherently PIC, no GOT/PLT,
 *     no absolute relocations.
 *   - The FNV constants are 64-bit immediates (movabs), encoded inline; they
 *     do NOT become .rodata.
 *
 * Computation: FNV-1a (64-bit) over the operand u64 array, then XOR the
 * per-run nonce. Order-sensitive and dependent on every operand, so the
 * host's independent replica matches ONLY if the device executed these exact
 * bytes over these exact operands with this exact nonce. The constants below
 * MUST match host_offload.c's local replica.
 */

#include "blob_abi.h"

uint64_t entry(void *argp)
{
    struct blob_args *a = (struct blob_args *)argp;

    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a 64-bit offset basis */
    for (uint64_t i = 0; i < a->n; i++) {
        h ^= a->data[i];
        h *= 0x100000001b3ULL;            /* FNV-1a 64-bit prime */
    }
    h ^= a->nonce;
    return h;
}
