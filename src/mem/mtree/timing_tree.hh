#ifndef __MEM_MTREE_TIMING_TREE_HH__
#define __MEM_MTREE_TIMING_TREE_HH__

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "base/types.hh"
#include "mem/mtree/abstract_tree.hh"

namespace gem5 {

/**
 * Implementation of an integrity tree. Currently assumes that the tree has
 * nodes that are all the same size, similar to the basic Merkle tree.
 * Additionally, there is a direct linear mapping between nodes and their
 * children/parent nodes.
 *
 * This is not actually a fully real tree, because the actual contents are not
 * being modeled. This is mostly in the interest of testing the timing of
 * behavior.
 *
 * The structure may look something like the following, with abstract
 * representation:
 *
 * ```
 *  [SECURE REGION]           (Root) -- Size: hashOutputSize
 *  ----------------------------^----------------------------------------------
 *  [INSECURE REGION]           | (hashInputSize -> hashOutputSize)
 *     Height = 1            (Block) -- Size: BLOCK_SIZE_BYTES or hashInputSize
 *                              |
 *                              | (The split here is based on arity.)
 *         +--------------------+-----------------+----------------+
 *         |                    |                 |                |
 *         |    Each Block:     |                 |                |
 *         | (hashInputSize ->  |                 |                |
 * Height=2|  hashOutputSize)   |                 |                |
 *         |                    |                 |                |
 *      (Block)              (Block)           (Block)          (Block)
 *                            (Each Block is BLOCK_SIZE_BYTES or hashInputSize)
 *                                .
 *                                .
 *                                .
 *                                .
 *  (Tree Leaves.)        (Block) (Block) (Block) ...... (Block)
 *    Height = height                     /     \
 *                                       /        \
 *                           __________/            \__________
 *                 _________/                                  \_____
 *               /              Each cache line is hashed.           \
 *              /             `arity` number of hashes are within     \
 *             [                         a single Block.               ]
 *             <---------------------------arity----------------------->
 *        .... (Cache Line -- Actual Data) (Cache Line) ... (Cache Line) ....
 * ```
 *
 * Each Block looks like:
 *
 * ```
 *     <------------------BLOCK_SIZE_BYTES/hashInputSize------------------->
 *     [                  |                  |          |                  ]
 *     [                  |                  |          |                  ]
 *     [  hashOutputSize  |  hashOutputSize  |  ......  |  hashOutputSize  ]
 *     [                  |                  |          |                  ]
 *     [                  |                  |          |                  ]
 *         (`arity` total hashes in one Block.)
 * ```
 */
class TimingTree : public AbstractIntegrityTree
{
  public:
    ////// Basic Functionality //////

    /**
     * Initializer for integrity tree based on a fixed arity and the total
     * amount of data being protected. The corresponding height of the tree is
     * calculated automatically.
     *
     * @param total_data Total data being protected in bytes.
     */
    TimingTree(unsigned int arity, uint64_t total_data);

    ~TimingTree();

    // void drawTree();

    void processWrite(size_t address, std::vector<unsigned char> newData)
      override;

    bool isMocking() override { return true; };

    /**
     * Process a write, and collect the amount of time this takes in total.
     * Returns the number of cycles taken for the write.
     */
    unsigned int processWriteTiming(size_t address);

  private:
    /**
     * Cache block size. The size of the input for hashing should likely be the
     * same as the cache line size.
     */
    const unsigned int BLOCK_SIZE_BYTES = 64;

    ////// Timing Information //////
    /**
     * Time to compute the MAC for a cacheline (AES, etc.)
     *
     * 40 cycles from suh03 and poisonivy16.
     */
    const unsigned int CYCLES_COMPUTE_HASH = 40;
    /**
     * Time to check whether some data exists in the metadata cache.
     *
     * TODO This value should be verified.
     */
    const unsigned int CYCLES_CHECK_EXIST_IN_CACHE = 5;
    /**
     * Time to read data from memory.
     *
     * Depends on the interconnect and metadata cache.
     * Assume for now no cache.
     *
     * TODO This value should be verified.
     */
    unsigned int CYCLES_READ_DATA = 80;
    /**
     * Time to read the secure root.
     *
     * Basically negligible time.
     */
    const unsigned int CYCLES_READ_ROOT = 0;
    /**
     * Time to write data from memory.
     *
     * Depends on the interconnect and metadata cache.
     * Assume for now no cache.
     *
     * TODO This value should be verified.
     */
    unsigned int CYCLES_WRITE_DATA = 80;
    /**
     * Time to compare a generated MAC (with the one stored in the parent
     * node).
     *
     * Basically negligible time.
     */
    const unsigned int CYCLES_COMPARE_MAC = 0;
    /**
     * Time to move a node from the read buffer to the cache (now that it has
     * been verified.)
     *
     * Basically negligible time.
     */
    const unsigned int CYCLES_MOVE_BUFFER_TO_CACHE = 0;
    /**
     * Time to update a node in the metadata cache with a new hash of its
     * child.
     *
     * Basically negligible time.
     */
    const unsigned int CYCLES_UPDATE_PARENT = 0;


    /**
     * Internal tracking of how large the `data` array is. (By number of array
     * indexes.)
     *
     * Note that this also represents the amount of data that the tree is
     * directly protecting.
     */
    size_t dataSize;

    /// @brief How many levels of the tree there are.
    unsigned int height;

    /// @brief How many children for each node in the tree.
    unsigned int arity;

    /**
     * Number of leaf nodes in the tree.
     */
    size_t leaves;

    /**
     * The size (in bytes) of how much data is input into the hashing function.
     */
    unsigned int hashInputSize;

    /**
     * The size (in bytes) of how much data is output from the hashing
     * function.
     */
    unsigned int hashOutputSize;

  // Assume i is the index of the heap.
  // Parent(i) = floor((i - 1)/arity)
  // Child(i) = (arity * i) + j, where 1 <= j < arity (multiple children)

  public:
    ////// API //////

    /**
     * Calculate the "location" of a tree node by its offset, in bytes.
     *
     * Consider the first node, 0, to be at offset 0, then the second node
     * to be at offset BLOCK_SIZE_BYTES, and so on.
     */
    uint64_t simulatedBlockOffset(size_t index) override;

    /**
     * Get the index of the parent block for a given block.
     */
    size_t parentBlockIndex(size_t index) override;

    /**
     * Get the index of the block (this will be a leaf) that corresponds to
     * a given address.
     *
     * NOTE: This assumes a valid address is used.
     *
     * It is assumed that there is a linear mapping between memory addresses
     * to leaf nodes, starting at the left-most leaf node.
     */
    size_t addressToBlockIndex(size_t address) override;

    /**
     * Determine if a given parent block index is an ancestor of a child block
     * index.
     */
    bool isAncestor(size_t parent, size_t child) override;

    /**
     * Returns the type of node a requested node is.
     */
    TreeNodeType getNodeType(size_t index) override;


    ////// TimingTree //////

  private:
    /**
     * Get the index of the first child block for a given block.
     */
    size_t firstChildBlockIndex(size_t index);

    /**
     * Get the index of a child block relative to its parent.
     *
     * Returns a value between 0 and `arity - 1`.
     */
    size_t relativeChildBlockIndex(size_t childIndex);

  public:
    /**
     * Determine if a given block index corresponds to a leaf.
     */
    bool isLeaf(size_t index);


  private:
    /**
     * Update a leaf block's hash by reading the data that corresponds to that
     * leaf.
     *
     * Returns the amount of time taken in cycles.
     */
    unsigned int updateLeafHash(size_t address);


    /**
     * Update a block's hash by reading its children.
     *
     * NOTE: This will read ALL children of a block, whether or not it was
     * actually changed.
     */
    void updateBlockHash(size_t index);

    /**
     * Update a block's hash with only the specified child.
     *
     * @param index The index of the block being updated.
     * @param childIndex The index of the child of the block that should be
     *                   referred to. Should be between 0 and `arity - 1`.
     *                   This index is also used to determine what portion of
     *                   the parent block's value is updated.
     * @returns The amount of time taken in cycles.
     */
    unsigned int updateBlockHashPartial(size_t index, size_t childIndex);

  public:
    ////// Statistics //////

    /**
     * Get the number of levels in the tree.
     */
    unsigned int statTreeHeight() override { return height; }

    /**
     * Get the arity of the tree.
     */
    unsigned int statTreeArity() override { return arity; }

    /**
     * Get the size of a single hash, in bytes.
     */
    unsigned int statOutputHashSize() override { return hashOutputSize; }

    /**
     * Get the total amount of data that is protected by the tree, in bytes.
     */
    long long statDataProtected() override;

    /**
     * Get the total amount of data in the data structure, in bytes.
     */
    long long statStructureSize() override;

    /**
     * Get the size of the full data structure relative to the amount of data
     * that is being protected.
     *
     * This calculates the data structure size *relative* to the protected data
     * size. So you could use the output of this function to say "You need X%
     * additional data to be able to protect this data."
     */
    double statDataOverheadRatio() override;
};

} // namespace gem5

#endif // __MEM_MTREE_TIMING_TREE_HH__
