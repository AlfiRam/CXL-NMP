#ifndef __BASE_ADDR_RANGE_LIST_HH__
#define __BASE_ADDR_RANGE_LIST_HH__

#include "base/addr_range.hh"

namespace gem5
{

// Extra utility functions for lists of AddrRange.

[[maybe_unused]]
static inline bool
rangeListContains(const AddrRangeList &base, const Addr &addr)
{
    for (const auto &range: base) {
        if (range.contains(addr)) return true;
    }
    return false;
}

/**
 * Assumes disjoint address ranges.
 */
[[maybe_unused]]
static inline Addr
rangeListSize(const AddrRangeList &base)
{
    Addr sum = 0;

    for (const auto &range: base) {
        sum += range.size();
    }
    return sum;
}

[[maybe_unused]]
static std::string
rangeListToString(const AddrRangeList &base)
{
    std::ostringstream str;

    ccprintf(str, "[");
    for (const auto &range: base) {
        ccprintf(str, "%s (%llu:%llu, size %llu), ",
                range.to_string(), range.start(), range.end(), range.size());
    }
    ccprintf(str, "]");

    return str.str();
}

} // namespace gem5

#endif // __BASE_ADDR_RANGE_LIST_HH__
