#include "mem/mtree/tree.hh"

#include <iomanip>

#include "base/trace.hh"
#include "debug/IntegrityTree.hh"
#include "mem/mtree/util.hh"

namespace gem5 {

Block::Block(unsigned int size)
{
  blockSize = size;
  value = new unsigned char[size];
}

Block::~Block()
{
  delete value;
}

std::ostream& operator<<(std::ostream& os, const Block& block)
{
  std::ios state(nullptr);

  os << "0x";

  state.copyfmt(os);
  for (size_t i = 0; i < block.blockSize; i++) {
    unsigned char byte = block.value[i];

    os << std::hex
       << std::setw(2)
       << std::setfill('0')
       << (static_cast<int>(byte) & 0xFF);
  }
  os.copyfmt(state);

  os << " (" << block.blockSize << " bytes)";

  return os;
}


void Block::setValue(unsigned int newValue)
{
  for (size_t i = 0; i < blockSize; i++) {
    // Take the last byte of the value.
    unsigned char byte = newValue & 0xFF;
    // std::cout << "Byte "
    //    << std::hex
    //    << std::setw(2)
    //    << std::setfill('0')
    //    << (unsigned int) byte
    //    << "// ";

    // Assign the byte to the end of the block's value array.
    value[blockSize - 1 - i] = byte;

    // Shift 8 bits (1 byte)
    newValue >>= 8;
  }
}

void Block::setValue(std::vector<unsigned char> newValue)
{
  // Indicate which byte we are writing to in the block's internal value.
  // NOTE: Assumes `block_size >= 1`.
  size_t byte = blockSize - 1;

  for (size_t i = 0; i < newValue.size(); i++)
  {
    unsigned char element = newValue[newValue.size() - 1 - i];
    value[byte] = element;

    // Stop if we have no more room in the block.
    if (byte == 0) {
      return;
    }

    byte--;
  }
}


void Block::setPartialValue(size_t offset, std::vector<unsigned char> newValue)
{
  // Indicate which byte we are writing to in the block's internal value.
  size_t byte = offset + newValue.size() - 1;

  for (size_t i = 0; i < newValue.size(); i++)
  {
    value[byte] = newValue[newValue.size() - 1 - i];

    // Stop if we have reached the beginning of the offset.
    if (byte == offset) {
      return;
    }

    byte--;
  }
}


IntegrityTree::IntegrityTree(unsigned int height, unsigned int arity) :
  height(height),
  arity(arity),
  hashInputSize(BLOCK_SIZE_BYTES),
  hashOutputSize(hashInputSize / arity)
{
  // Size of the array depends on the arity and the number of tree levels.
  // Level 1: 1 Node (top of tree -- but root is still one hash up)
  // Level 2: arity
  // Level 3: arity**2
  // Level 4: arity**3
  // ...
  // Total: (arity**(treeLevels))/(arity - 1)

  dataSize = integerPower(arity, height)/(arity - 1);

  data = new Block*[dataSize];
  for (size_t i = 0; i < dataSize; i++) {
    data[i] = new Block(BLOCK_SIZE_BYTES);
  }

  // Allocate the root, which is separate from the top-level node of the tree.
  root = new Block(hashOutputSize);
}

IntegrityTree::~IntegrityTree() {
  for (size_t i = 0; i < dataSize; i++) {
    delete data[i];
  }

  delete[] data;

  delete root;
}

void IntegrityTree::drawTree() {
  unsigned int nodesOnLevel = 1;
  size_t i = 0;

  for (unsigned int level = 0; level < height; level++) {

    for (unsigned int node = 0; node < nodesOnLevel; node++) {
      std::cout << "[ " << data[i] << " ]";
      i++;
    }
    std::cout << std::endl;

    nodesOnLevel *= arity;
  }
}


void IntegrityTree::processWrite(
  size_t address,
  std::vector<unsigned char> newData
) {
  std::cout << "====================================" << std::endl;
  std::cout << "Processing write at address " << address << "." << std::endl;
  std::cout << "Input data: ";
  printHex(&newData[0], newData.size());

  // Calculate which leaf corresponds to this request's address.
  size_t leafIndex = addressToBlockIndex(address);
  Block* leaf = data[leafIndex];

  // Then we'll probably also want to know exactly which portion of this block
  // is relevant to this address.
  size_t blockOffset = (address / hashInputSize) % arity;
  std::cout << "The portion of the leaf for this is hash #"
    << blockOffset << std::endl;

  // Time to start hashing!
  // Update the hash for the leaf that directly corresponds to this address.
  // MD5 hash;
  // hash(&newData[0], newData.size());

  unsigned char *outputHash = new unsigned char[hashOutputSize];

  // Copy the hash to a buffer.
  // hash.getHash(outputHash);

  std::cout << "Hash value of cache block: ";
  printHex(outputHash, hashOutputSize);

  // TODO probably want to make a function later that updates a leaf block
  // itself

  // Copy the hash from the buffer to the leaf.
  size_t byteOffset = blockOffset * hashOutputSize;
  for (size_t i = 0; i < hashOutputSize; i++) {
    leaf->value[byteOffset + i] = outputHash[i];
  }
  std::cout << "Contents of leaf block (#" << leafIndex << "): ";
  printHex(leaf->value, leaf->blockSize);

  // Update up the rest of the tree.
  size_t childIndex = leafIndex;
  size_t parentIndex = parentBlockIndex(leafIndex);

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

  Block* topNode = data[0];

  // hash(&topNode->value[0], hashInputSize);
  // hash.getHash(outputHash);


  std::cout << "Root hash value: ";

  printHex(outputHash, hashOutputSize);

  // Copy the hash from the buffer to the root.
  for (size_t i = 0; i < hashOutputSize; i++) {
    root->value[i] = outputHash[i];
  }

  std::cout << "Updated root hash." << std::endl;
}


size_t IntegrityTree::parentBlockIndex(size_t index) {
  if (index == 0) {
    // There is no parent.
    throw -1;
  }

  return (index - 1) / arity;
}


size_t IntegrityTree::firstChildBlockIndex(size_t index) {
  return (arity * index) + 1;
}


size_t IntegrityTree::relativeChildBlockIndex(size_t childIndex) {
  // Get the parent index, find its first child, then get the difference
  // between the requested child and the first child.

  size_t parentIndex = parentBlockIndex(childIndex);
  size_t firstChildIndex = firstChildBlockIndex(parentIndex);

  return childIndex - firstChildIndex;
}


size_t IntegrityTree::addressToBlockIndex(size_t address) {
  // Get the index of the first leaf node.
  size_t firstLeafIndex = integerPower(arity, height - 1)/(arity - 1);
  DPRINTF(IntegrityTree, "%s: We have %d total nodes.\n", __func__, dataSize);
  DPRINTF(IntegrityTree, "%s: The first leaf is at index %d.\n",
    __func__, firstLeafIndex);
  DPRINTF(IntegrityTree, "%s: The arity is %d.\n", __func__, arity);

  // unsigned int nodesOnLevel = 1;
  // unsigned int firstNodeIndex = 0;
  // size_t i = 0;

  // for (unsigned int level = 0; level < height; level++) {

  //   std::cout << "There are "
  //      << nodesOnLevel << " nodes on level " << level << std::endl;
  //   std::cout << "The first index of this level is "
  //      << firstNodeIndex << std::endl;

  //   firstNodeIndex += nodesOnLevel;
  //   nodesOnLevel *= arity;
  // }

  // Offset this index by the amount of memory covered up to this address.

  // We have the first leaf, so now we need to calculate the offset.
  // One leaf block covers `inputHashSize * arity` bytes.
  // So take the address and divide by this amount.
  size_t leafOffset = address / (hashInputSize * arity);
  DPRINTF(IntegrityTree,
    "%s: The leaf that corresponds to this address is #%d\n",
    __func__, leafOffset);

  return firstLeafIndex + leafOffset;
}


void IntegrityTree::updateBlockHash(size_t index) {
  for (size_t i = 0; i < arity; i++) {
    updateBlockHashPartial(index, i);
  }
}


void IntegrityTree::updateBlockHashPartial(size_t index, size_t childIndex) {
  // The block being updated.
  Block* block = data[index];
  DPRINTF(IntegrityTree, "%s: Updating block %d.\n", index);

  // The child block that has changed and needs to propagate its hash up.
  Block* childBlock = data[firstChildBlockIndex(index) + childIndex];

  // Determine where to start changing data in the updated block.
  size_t byteOffset = childIndex * hashOutputSize;

  DPRINTF(IntegrityTree, "%s: Updating bytes #%d through #%d.\n",
    __func__, byteOffset, (byteOffset + hashOutputSize - 1));

  // Time to start hashing!
  // Get the child block's data and hash it.
  // MD5 hash;
  // hash(&childBlock->value[0], hashInputSize);

  // Copy the hash to the parent block.
  // hash.getHash(&(block->value[byteOffset]));

  // std::cout << "Hash value: ";
  // printHex(&(block->value[byteOffset]), hashOutputSize);

  // std::cout << "New contents of block #" << index << ":" << std::endl;
  // std::cout << *block << std::endl;
}


long long IntegrityTree::statDataProtected() {
  // Assuming that in the basic version, each leaf contains `arity` number of
  // hashes for real data.

  // Number of leaves.
  long long lastLevelNodes = integerPower(arity, height - 1);

  // Consider the total number of hashes created by the set of leaves.
  long long leafHashes = lastLevelNodes * arity;

  // Consider the amount of data represented by a single hash.
  return leafHashes * hashInputSize;
}

long long IntegrityTree::statStructureSize() {
  return dataSize * BLOCK_SIZE_BYTES;
}

double IntegrityTree::statDataOverheadRatio() {
  return (double) statStructureSize() / statDataProtected();
}

} // namespace gem5
