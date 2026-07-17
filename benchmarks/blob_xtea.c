/*
 * blob_xtea.c — XTEA cipher blob for OP_EXEC_BLOB.
 *
 * Demonstrates the device executing security-critical, key-dependent crypto
 * (the enclave use case): the host ships a 128-bit key + 64-bit plaintext
 * block, the device copies these bytes into an executable page and CALLS this
 * function, which XTEA-encrypts the block under the key and returns the
 * ciphertext. The host independently encrypts the same block and compares.
 *
 * Same freestanding discipline as blob_fnv.c — see benchmarks/Makefile for
 * the build + the objdump verify gate (entry at .text offset 0, ZERO
 * relocations). XTEA passes that gate BY CONSTRUCTION: it is only u32 shifts,
 * adds, and XORs plus `& 3` (bitwise, not modulo), with the delta constant
 * 0x9E3779B9 as an immediate. No tables, no .rodata, no libc, no
 * division/modulo, no external calls -> relocation-free position-independent
 * raw .text, entry at offset 0 (single function in this TU).
 *
 * ALL arithmetic is uint32_t (unsigned): XTEA needs LOGICAL right shifts and
 * mod-2^32 wraparound. `key[4]` is a mutable STACK array filled at runtime
 * from the operands (never .rodata); `key[sum & 3]` is a stack-relative
 * index. The host's replica in host_offload.c MUST use identical constants,
 * rounds, types, and packing.
 *
 * Arg contract (blob_abi.h, one u32 per u64 operand slot, low 32 bits):
 *   data[0..3] = key words k0..k3   data[4] = v0   data[5] = v1   (n = 6)
 * Result packing: result = v0 | ((uint64_t)v1 << 32) after encryption.
 * arg2/nonce is unused (the 128-bit key is the secret).
 */

#include "blob_abi.h"

uint64_t entry(void *argp)
{
    struct blob_args *a = (struct blob_args *)argp;

    /* Runtime-filled stack array (explicit assignments, not a copy that
     * could emit memcpy) — never lands in .rodata. */
    uint32_t key[4];
    key[0] = (uint32_t)a->data[0];
    key[1] = (uint32_t)a->data[1];
    key[2] = (uint32_t)a->data[2];
    key[3] = (uint32_t)a->data[3];

    uint32_t v0 = (uint32_t)a->data[4];
    uint32_t v1 = (uint32_t)a->data[5];

    uint32_t sum = 0;
    const uint32_t delta = 0x9E3779B9u;   /* immediate, not .rodata */

    for (int i = 0; i < 32; i++) {        /* standard 32-round XTEA */
        v0  += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1  += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }

    return ((uint64_t)v1 << 32) | (uint64_t)v0;
}
