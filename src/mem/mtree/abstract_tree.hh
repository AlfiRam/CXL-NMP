#ifndef __MEM_MTREE_ABSTRACT_TREE_HH__
#define __MEM_MTREE_ABSTRACT_TREE_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gem5 {

class AbstractIntegrityTree
{
  public:
    ////// Basic Functionality //////

    virtual ~AbstractIntegrityTree() {};

    virtual void processWrite(
      size_t address,
      std::vector<unsigned char> newData
    ) = 0;

    /**
     * Returns whether this tree is actually storing data, or is just here to
     * simulate other properties about the tree.
     */
    virtual bool isMocking() = 0;

    ////// API //////

    /**
     * Used to identify different type of integrity tree data, based on the
     * exact structure and layout being used.
     */
    enum TreeNodeType
    {
      /// The basic tree node.
      TreeNode = 0,
      Counter = 1,
      MAC = 2,
      TREE_NODE_TYPE_COUNT
    };

    static const char* treeNodeStrings[TREE_NODE_TYPE_COUNT];

    /**
     * Calculate the "location" of a tree node by its offset, in bytes.
     *
     * Consider the first node, 0, to be at offset 0, then the second node
     * to be at offset BLOCK_SIZE_BYTES, and so on.
     */
    virtual uint64_t simulatedBlockOffset(size_t index) = 0;

    /**
     * Get the index of the parent block for a given block.
     */
    virtual size_t parentBlockIndex(size_t index) = 0;

    /**
     * Get the index of the block (this will be a leaf) that corresponds to
     * a given address.
     *
     * NOTE: This assumes a valid address is used.
     *
     * It is assumed that there is a linear mapping between memory addresses
     * to leaf nodes, starting at the left-most leaf node.
     */
    virtual size_t addressToBlockIndex(size_t address) = 0;

    /**
     * Determine if a given parent block index is an ancestor of a child block
     * index.
     */
    virtual bool isAncestor(size_t parent, size_t child) = 0;

    /**
     * Returns the type of node a requested node is.
     */
    virtual TreeNodeType getNodeType(size_t index) = 0;


    ////// Statistics //////

    /**
     * Get the number of levels in the tree.
     */
    virtual unsigned int statTreeHeight() = 0;

    /**
     * Get the arity of the tree.
     */
    virtual unsigned int statTreeArity() = 0;

    /**
     * Get the size of a single hash, in bytes.
     */
    virtual unsigned int statOutputHashSize() = 0;

    /**
     * Get the total amount of data that is protected by the tree, in bytes.
     */
    virtual long long statDataProtected() = 0;

    /**
     * Get the total amount of data in the data structure, in bytes.
     */
    virtual long long statStructureSize() = 0;

    /**
     * Get the size of the full data structure relative to the amount of data
     * that is being protected.
     *
     * This calculates the data structure size *relative* to the protected data
     * size. So you could use the output of this function to say "You need X%
     * additional data to be able to protect this data."
     */
    virtual double statDataOverheadRatio() = 0;
};

} // namespace gem5

#endif // __MEM_MTREE_ABSTRACT_TREE_HH__
