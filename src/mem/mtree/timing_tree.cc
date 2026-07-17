#include "mem/mtree/timing_tree.hh"

#include <iomanip>

#include "base/trace.hh"
#include "debug/TimingTree.hh"
#include "debug/TimingTreeInit.hh"
#include "mem/mtree/util.hh"

namespace gem5 {

TimingTree::TimingTree(unsigned int arity, uint64_t total_data) :
  arity(arity),
  hashInputSize(BLOCK_SIZE_BYTES),
  hashOutputSize(hashInputSize / arity)
{
  // Note this should be in sync with the Python version

  assert(arity > 0);

  // Calculate the number of leaves needed to get this much data

  // Consider the total number of hashes necessary to cover this area.
  long long leafHashes = total_data / hashInputSize;
  // Handle possible extra remainder.
  if (total_data % hashInputSize != 0) leafHashes++;
  DPRINTF(TimingTreeInit, "%s: %lld total leaf hashes needed.\n", __func__,
      leafHashes);

  long long lastLevelNodes = leafHashes / arity;
  // Handle possible extra remainder.
  if (leafHashes % arity != 0) lastLevelNodes++;
  DPRINTF(TimingTreeInit, "%s: %lld last level nodes.\n",
    __func__, lastLevelNodes);
  leaves = lastLevelNodes;

  // Calculate how many levels you need to get that many leaves in the tree.
  unsigned int levels = integerLog(lastLevelNodes, arity) + 1;
  height = levels;
  DPRINTF(TimingTreeInit, "%s: The height is %u.\n", __func__, levels);

  // Initialize the data size parameter. Take the number of nodes for all the
  // fully-filled levels, then add the number of leaf nodes.
  dataSize = integerPower(arity, levels - 1)/(arity - 1);
  dataSize += lastLevelNodes;
  DPRINTF(TimingTreeInit, "%s: Total number of nodes is %u.\n",
    __func__, dataSize);

  DPRINTF(TimingTreeInit, "%s: Total space protected by tree: %llu bytes. "
    "(Requested: %llu bytes.)\n",
    __func__, statDataProtected(), total_data);

  DPRINTF(TimingTreeInit, "%s: Total space taken by tree: %llu bytes.\n",
    __func__, statStructureSize());
}

TimingTree::~TimingTree() {
}

void TimingTree::processWrite(
  size_t address,
  std::vector<unsigned char> newData
) {
  std::cout << "====================================" << std::endl;
  std::cout << "Processing write at address " << address << "." << std::endl;
  std::cout << "Input data: ";
  printHex(&newData[0], newData.size());

  // Time to start hashing!
  // Update the hash for the leaf that directly corresponds to this address.
  updateLeafHash(address);

  // Update up the rest of the tree.
  size_t childIndex = addressToBlockIndex(address);
  size_t parentIndex = parentBlockIndex(childIndex);

  while (true) {
    try {
      // Find exactly where the block is relative to its parent.
      size_t relativeChildIndex = relativeChildBlockIndex(childIndex);

      // Partially update the parent block based on this child.
      updateBlockHashPartial(parentIndex, relativeChildIndex);

      // Ascend the tree up one level.
      childIndex = parentIndex;
      parentIndex = parentBlockIndex(parentIndex);
    }
    catch (int exception) {
      // No more parents. Stop ascending the tree.
      break;
    }
  }

  // Update top node (leads to secure root).
  // (input one node, output a single hash)
}


unsigned int TimingTree::processWriteTiming(size_t address) {
  // Keep track of the amount of time required to complete write (in cycles).
  unsigned int totalTime = 0;

  std::cout << "====================================" << std::endl;
  std::cout << "Processing write at address " << address << "." << std::endl;

  // Time to start hashing!
  // Update the hash for the leaf that directly corresponds to this address.
  totalTime += updateLeafHash(address);

  // Update up the rest of the tree.
  size_t childIndex = addressToBlockIndex(address);
  size_t parentIndex = parentBlockIndex(childIndex);

  while (true) {
    try {
      // Find exactly where the block is relative to its parent.
      size_t relativeChildIndex = relativeChildBlockIndex(childIndex);

      // Partially update the parent block based on this child.
      totalTime += updateBlockHashPartial(parentIndex, relativeChildIndex);

      // Ascend the tree up one level.
      childIndex = parentIndex;
      parentIndex = parentBlockIndex(parentIndex);
    }
    catch (int exception) {
      // No more parents. Stop ascending the tree.
      break;
    }
  }

  // Update top node (leads to secure root).
  // (input one node, output a single hash)

  return totalTime;
}


size_t TimingTree::parentBlockIndex(size_t index) {
  if (index == 0) {
    // There is no parent.
    throw -1;
  }

  return (index - 1) / arity;
}


size_t TimingTree::firstChildBlockIndex(size_t index) {
  return (arity * index) + 1;
}


size_t TimingTree::relativeChildBlockIndex(size_t childIndex) {
  // Get the parent index, find its first child, then get the difference
  // between the requested child and the first child.

  size_t parentIndex = parentBlockIndex(childIndex);
  size_t firstChildIndex = firstChildBlockIndex(parentIndex);

  return childIndex - firstChildIndex;
}


bool TimingTree::isLeaf(size_t index) {
  size_t firstLeafIndex = integerPower(arity, height - 1)/(arity - 1);
  return (index >= firstLeafIndex);
}


bool TimingTree::isAncestor(size_t parent, size_t child) {
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
TimingTree::getNodeType(size_t index) {
  // All nodes in the TimingTree are regular tree nodes.
  return TreeNodeType::TreeNode;
}


size_t TimingTree::addressToBlockIndex(size_t address) {
  // Get the index of the first leaf node.
  size_t firstLeafIndex = integerPower(arity, height - 1)/(arity - 1);
  // DPRINTF(TimingTree, "%s: We have %d total nodes.\n", __func__, dataSize);
  // DPRINTF(TimingTree, "%s: The first leaf is at index %d.\n",
        // __func__, firstLeafIndex);
  // DPRINTF(TimingTree, "%s: The arity is %d.\n", __func__, arity);

  // unsigned int nodesOnLevel = 1;
  // unsigned int firstNodeIndex = 0;
  // size_t i = 0;

  // for (unsigned int level = 0; level < height; level++) {

  //   std::cout << "There are "
  //        << nodesOnLevel << " nodes on level " << level << std::endl;
  //   std::cout << "The first index of this level is "
  //        << firstNodeIndex << std::endl;

  //   firstNodeIndex += nodesOnLevel;
  //   nodesOnLevel *= arity;
  // }

  // Adjust the address to ensure it is aligned to the cache line size.
  address = address & ~(BLOCK_SIZE_BYTES - 1);

  // Offset this index by the amount of memory covered up to this address.

  // We have the first leaf, so now we need to calculate the offset.
  // One leaf block covers `inputHashSize * arity` bytes.
  // So take the address and divide by this amount.
  size_t leafOffset = address / (hashInputSize * arity);
  // DPRINTF(TimingTree,
      // "%s: The leaf that corresponds to this address is #%d\n",
      // __func__, leafOffset);

  return firstLeafIndex + leafOffset;
}



uint64_t TimingTree::simulatedBlockOffset(size_t index) {
  return (index * BLOCK_SIZE_BYTES);
}


unsigned int TimingTree::updateLeafHash(size_t address) {
  std::cout << "====================================" << std::endl;
  std::cout << "Processing write at address " << address << "." << std::endl;

  // Calculate which leaf corresponds to this request's address.
  [[maybe_unused]] size_t leafIndex = addressToBlockIndex(address);

  // Find exactly which portion of this block is relevant to this address.
  size_t blockOffset = (address / hashInputSize) % arity;
  std::cout << "The portion of the leaf for this is hash #"
    << blockOffset << std::endl;

  // Time to start hashing!
  // Update the hash for the leaf that directly corresponds to this address.
  // outputHash = hash(newData)

  // leaf between [`byteOffset` and `byteOffset + hashOutputSize`) = outputHash

  // total time = identify block + load + hash + store
  return CYCLES_READ_DATA + CYCLES_COMPUTE_HASH + CYCLES_WRITE_DATA;
}


void TimingTree::updateBlockHash(size_t index) {
  for (size_t i = 0; i < arity; i++) {
    updateBlockHashPartial(index, i);
  }
}


unsigned int TimingTree::updateBlockHashPartial(
  size_t index,
  size_t childIndex
) {
  // The block being updated.
  std::cout << "Updating block " << index << "." << std::endl;

  // The child block that has changed and needs to propagate its hash up.
  [[maybe_unused]] size_t childBlock = firstChildBlockIndex(index)
                                        + childIndex;

  // Determine where to start changing data in the updated block.
  size_t byteOffset = childIndex * hashOutputSize;

  std::cout << "Updating bytes #" << byteOffset << " through #"
    << (byteOffset + hashOutputSize - 1) << "." << std::endl;

  // Time to start hashing!
  // Get the child block's data and hash it.
  // hash(childBlock)

  // Copy the hash to the parent block.
  // block[byteOffset] = hash(childBlock)
  return CYCLES_READ_DATA + CYCLES_COMPUTE_HASH + CYCLES_WRITE_DATA;
}


long long TimingTree::statDataProtected() {
  // NOTE Sync with Python

  // Assuming that in the basic version, each leaf contains `arity` number of
  // hashes for real data.

  // Consider the total number of hashes created by the set of leaves.
  long long leafHashes = leaves * arity;

  // Consider the amount of data represented by a single hash.
  return leafHashes * hashInputSize;
}

long long TimingTree::statStructureSize() {
  // NOTE Sync with Python

  return dataSize * BLOCK_SIZE_BYTES;
}

double TimingTree::statDataOverheadRatio() {
  return (double) statStructureSize() / statDataProtected();
}

} // namespace gem5
