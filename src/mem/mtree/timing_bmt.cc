#include "mem/mtree/timing_bmt.hh"

#include "base/trace.hh"
#include "debug/TimingBmt.hh"
#include "debug/TimingBmtInit.hh"
#include "mem/mtree/util.hh"

namespace gem5 {

TimingBmt::TimingBmt(unsigned int arity, uint64_t total_data) :
    arity(arity),
    hashInputSize(BLOCK_SIZE_BYTES),
    hashOutputSize(hashInputSize / arity)
{
    dataSize = 0;

    assert(arity > 0);

    // Note that this should be in sync with the Python version

    // Calculate number of MACs and nodes this requires.
    // Each cache line is turned into a MAC.
    macCount = ceilDiv(total_data, hashInputSize);
    DPRINTF(TimingBmtInit, "%s: Number of MACs: %lld\n", __func__, macCount);
    // `arity` number of MACs can fit in a single node.
    macNodes = ceilDiv(macCount, arity);
    DPRINTF(TimingBmtInit, "%s: Number of MAC nodes: %lld\n",
        __func__, macNodes);
    dataSize += macNodes;

    // Calculate number of counters and counter nodes this requires.
    // Each cache line has a counter.
    long long counter_count = ceilDiv(total_data, hashInputSize);
    DPRINTF(TimingBmtInit, "%s: Number of (minor) counters: %lld\n",
        __func__, counter_count);
    // In a split counter configuration, we consider a 64-byte / 512-bit cache
    // line size. Each "counter node" contains a 64-bit major counter and
    // enough minor counters to cover a single 4KB page.
    // Consider a 512-bit cacheline = 64-bit major counter + 448 bits of minor
    // counters.
    // If one page is 4KB, and each counter is for a 64-byte cacheline, you
    // need 64 total cachelines for a page to fit within that 448 bits. This
    // gives 7 bits for each counter.

    unsigned int minor_counter_space = (BLOCK_SIZE_BYTES * 8) -
        majorCounterBits;
    unsigned int counters_per_page = PAGE_SIZE_BYTES / hashInputSize;
    minorCounterBits = minor_counter_space / counters_per_page;
    DPRINTF(TimingBmtInit, "%s: Major counter size: %u bits\n",
        __func__, majorCounterBits);
    DPRINTF(TimingBmtInit, "%s: Minor counter size: %u bits\n",
        __func__, minorCounterBits);

    // Ensure an invalid configuration is not created.
    assert(majorCounterBits + (minorCounterBits * counters_per_page) <=
        (BLOCK_SIZE_BYTES * 8));

    counterNodes = ceilDiv(counter_count, counters_per_page);
    DPRINTF(TimingBmtInit, "%s: Number of counter nodes: %lld\n",
        __func__, counterNodes);
    dataSize += counterNodes;

    // Calculate the number of nodes used to create the tree that protects
    // counters.

    leaves = ceilDiv(counterNodes, arity);
    height = integerLog(leaves, arity) + 1;

    DPRINTF(TimingBmtInit, "%s: The height of the tree is %u\n",
        __func__, height);

    long long non_leaf_nodes = integerPower(arity, height - 1)/(arity - 1);
    dataSize += non_leaf_nodes;
    dataSize += leaves;
    treeNodes = non_leaf_nodes + leaves;

    // Make sure nothing weird happened.
    assert(treeNodes > 0);
    assert(counterNodes > 0);
    assert(macNodes > 0);
    assert(dataSize == treeNodes + counterNodes + macNodes);

    DPRINTF(TimingBmtInit, "%s: Total number of nodes is %llu.\n",
        __func__, dataSize);

    DPRINTF(TimingBmtInit, "%s: Total space protected by tree: %llu bytes. "
        "(Requested: %llu bytes.)\n",
        __func__, statDataProtected(), total_data);

    DPRINTF(TimingBmtInit,
        "%s: Total space taken by tree nodes: %llu bytes.\n",
        __func__, (treeNodes * BLOCK_SIZE_BYTES));
    DPRINTF(TimingBmtInit,
        "%s: Total space taken by counter nodes: %llu bytes.\n",
        __func__, (counterNodes * BLOCK_SIZE_BYTES));
    DPRINTF(TimingBmtInit,
        "%s: Total space taken by MAC nodes: %llu bytes.\n",
        __func__, (macNodes * BLOCK_SIZE_BYTES));
    DPRINTF(TimingBmtInit,
        "%s: Total space taken by tree: %llu bytes.\n",
        __func__, statStructureSize());
}

TimingBmt::~TimingBmt() {
}

size_t
TimingBmt::indexOfType(size_t index)
{
    auto type = getNodeType(index);

    switch (type) {
        case TreeNodeType::TreeNode:
        return index;
        break;

        case TreeNodeType::Counter:
        return index - treeNodes;
        break;

        case TreeNodeType::MAC:
        return index - treeNodes - counterNodes;
        break;

        default:
        panic("Unexpected tree node type.");
    }

    // Default return value to please the compiler.
    return 0;
}

size_t
TimingBmt::firstOfType(AbstractIntegrityTree::TreeNodeType type)
{
    switch (type) {
        case TreeNodeType::TreeNode:
        return 0;
        break;

        case TreeNodeType::Counter:
        return treeNodes;
        break;

        case TreeNodeType::MAC:
        return treeNodes + counterNodes;
        break;

        default:
        panic("Unexpected tree node type.");
    }

    // Default return value to please the compiler.
    return 0;
}

uint64_t
TimingBmt::simulatedBlockOffset(size_t index)
{
    return index * BLOCK_SIZE_BYTES;
}

size_t
TimingBmt::parentBlockIndex(size_t index)
{
    auto type = getNodeType(index);

    switch (type) {
        case TreeNodeType::TreeNode:
        // This is a more standard tree traversal using the usual methods.

        // The first node in the tree is considered the root, and has no
        // parent.
        assert(index != firstOfType(TreeNodeType::TreeNode));

        return firstOfType(TreeNodeType::TreeNode) +
            ((indexOfType(index) - 1) / arity);
        break;

        case TreeNodeType::Counter:
        // Consider the parent of a counter node to be the tree leaf node that
        // is protecting it.

        return firstOfType(TreeNodeType::TreeNode) +
            (indexOfType(index) / arity);
        break;

        case TreeNodeType::MAC:
        // For now, we will consider the "parent" of a MAC to be the counter
        // that helps generate the MAC.

        auto macs_in_node = arity;
        auto counters_in_node = PAGE_SIZE_BYTES / hashInputSize;

        // Both the list of MACs and list of counters are mapped the same way,
        // but the MACs are on a different scale from MACs. This represents the
        // number of MAC nodes that correspond to one counter node.
        assert(counters_in_node > macs_in_node);
        auto scale_factor = counters_in_node / macs_in_node;

        return firstOfType(TreeNodeType::Counter) +
            (indexOfType(index) / scale_factor);
        break;
    }

    panic("Unexpected tree node type.");
    return 0;
}

size_t
TimingBmt::addressToBlockIndex(size_t address)
{
    // Assume for now that the direct parent of a data node is the MAC.

    // Adjust the address to ensure it is aligned to the cache line size.
    address = address & ~(BLOCK_SIZE_BYTES - 1);

    size_t offset = address / (hashInputSize * arity);

    // DPRINTF(TimingTree,
        // "%s: The leaf that corresponds to this address is #%d\n",
        // __func__, offset);

    return firstOfType(TreeNodeType::MAC) + offset;
}

bool
TimingBmt::isAncestor(size_t parent, size_t child)
{
    assert(parent != child);

    while (parent < child) {
        child = parentBlockIndex(child);
        if (child < parent) {
            // Child hopped to the same level as or above the parent.
            // Not an ancestor.
            return false;
        } else if (child == parent) {
            // This is an ancestor.
            return true;
        }
    }

    return false;
}



AbstractIntegrityTree::TreeNodeType
TimingBmt::getNodeType(size_t index)
{
    assert(index < dataSize);

    // Tree nodes first, then counters, then MACs

    if (index < treeNodes) {
        return TreeNodeType::TreeNode;
    } else if (index < treeNodes + counterNodes) {
        return TreeNodeType::Counter;
    } else {
        return TreeNodeType::MAC;
    }
}



////// Statistics //////

unsigned int
TimingBmt::statTreeHeight()
{
    return height;
}

unsigned int
TimingBmt::statTreeArity()
{
    return arity;
}

unsigned int
TimingBmt::statOutputHashSize()
{
    return hashOutputSize;
}

long long
TimingBmt::statDataProtected()
{
    // NOTE Sync with Python
    return macCount * hashInputSize;
}

long long
TimingBmt::statStructureSize()
{
    // NOTE Sync with Python
    return dataSize * BLOCK_SIZE_BYTES;
}

double
TimingBmt::statDataOverheadRatio()
{
    return (double) statStructureSize() / statDataProtected();
}

} // namespace gem5
