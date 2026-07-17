#include "mem/mtree/util.hh"

#include <cmath>
#include <iomanip>
#include <iostream>

namespace gem5 {

long long integerPower(int base, int exponent) {
  long long result = 1;

  for (int i = 0; i < exponent; i++) {
    result *= base;
  }

  return result;
}

long long integerLog(long long num, unsigned int base) {
  // C++ library only provides the natural logarithm (base e), so this will
  // involve an additional change of base calculation.
  long double log_calculation = std::log((long double) num) /
                                  std::log((long double) base);

  // Round up to the nearest integer.
  log_calculation = ceil(log_calculation);

  return (long long) log_calculation;
}

long long ceilDiv(long long numerator, long long denominator) {
  long long result;

  result = numerator / denominator;
  if (numerator % denominator != 0) {
    result++;
  }

  return result;
}

void printHex(unsigned char* value, size_t length) {
  std::ios originalState(nullptr);

  std::cout << "0x";

  originalState.copyfmt(std::cout);
  for (size_t i = 0; i < length; i++) {
    unsigned char byte = value[i];

    std::cout << std::hex
              << std::setw(2)
              << std::setfill('0')
              << (static_cast<int>(byte) & 0xFF);
  }
  std::cout.copyfmt(originalState);

  std::cout << " (" << length << " bytes)" << std::endl;
}

} // namespace gem5
