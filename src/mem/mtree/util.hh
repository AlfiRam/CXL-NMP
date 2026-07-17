#ifndef __MEM_MTREE_UTIL_HH__
#define __MEM_MTREE_UTIL_HH__

#include <cstddef>

namespace gem5 {

long long integerPower(int base, int exponent);

/**
 * Calculate the logarithm of `num` with base `base`. If the result is not an
 * integer, it is rounded up to the next integer.
 */
long long integerLog(long long num, unsigned int base);

/**
 * Divide two integers, and return the resulting integer value.
 *
 * If the division results in a remainder, the ceil of the result is returned.
 */
long long ceilDiv(long long numerator, long long denominator);

/**
 * Print an array of character values as a hexadecimal string.
 */
void printHex(unsigned char* value, size_t length);

} // namespace gem5

#endif // __MEM_MTREE_UTIL_HH__
