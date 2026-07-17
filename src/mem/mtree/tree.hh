#ifndef __MEM_MTREE_TREE_HH__
#define __MEM_MTREE_TREE_HH__

#include <cstddef>
#include <iostream>
#include <vector>

#include "mem/mtree/abstract_tree.hh"

namespace gem5 {

class Block
{
  public:
    /**
     * The number of bytes in a single block.
     */
    size_t blockSize;

    /**
     * Array representing the bytes in the block. One array index per byte.
     * This is structured such that the most significant byte is at the
     * beginning of the array, and the least significant byte is at the end
     * (largest array index).
     */
    unsigned char* value;

    /**
     * Create a cache block of `size` bytes.
     */
    Block(unsigned int size);

    ~Block();

    /**
     * Print the contents of the block, in hexadecimal notation.
     */
    friend std::ostream& operator<<(std::ostream& os, const Block& block);

    /**
     * Set the internal value of the block to `newValue`. This is placed at
     * the end of the block value (higher array index).
     */
    void setValue(unsigned int newValue);

    /**
     * Set the internal value of the block to `newValue`, as an array. This is
     * placed in-order, from the end of the `newValue` array to the beginning,
     * copying values to the end (least significant) of the internal value,
     * moving backwards.
     */
    void setValue(std::vector<unsigned char> newValue);

    /**
     * Change a portion of the internal value of the block to `newValue`. This
     * is placed in-order, starting from the offset specified in bytes, copying
     * values to the end (least significant) of the internal value, moving
     * backwards and stopping at the point of the offset.
     */
    void setPartialValue(size_t offset, std::vector<unsigned char> newValue);
};

/**
 * Implementation of an integrity tree. Currently assumes that the tree has
 * nodes that are all the same size, similar to the basic Merkle tree.
 * Additionally, there is a direct linear mapping between nodes and their
 * children/parent nodes.
 *
 * This is a starting point for more complex integrity tree structures.
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
class IntegrityTree : AbstractIntegrityTree
{
  public:
    ////// Basic Functionality //////

    IntegrityTree(unsigned int height = 5, unsigned int arity = 4);

    ~IntegrityTree();

    void drawTree();

    void processWrite(size_t address, std::vector<unsigned char> newData)
      override;

    bool isMocking() override { return false; };

  private:
    /**
     * Cache block size. The size of the input for hashing should likely be the
     * same as the cache line size.
     */
    const unsigned int BLOCK_SIZE_BYTES = 64;

    // Input hash size: 64 bytes, sometimes 128 bytes (cache line size)
    // Output hash size: about 128 bits/16 bytes. (That's arity = 4 with 64
    // byte blocks)

    Block* root;

    /// @brief Data structure that actually contains the tree data. Like a
    /// heap.
    Block** data;

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

    /**
     * Get the index of the parent block for a given block.
     */
    size_t parentBlockIndex(size_t index);

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

    /**
     * Get the index of the block (this will be a leaf) that corresponds to
     * a given address.
     *
     * NOTE: This assumes a valid (aligned) address is used.
     *
     * It is assumed that there is a linear mapping between memory addresses
     * to leaf nodes, starting at the left-most leaf node.
     */
    size_t addressToBlockIndex(size_t address);


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
     */
    void updateBlockHashPartial(size_t index, size_t childIndex);

  public:
    ////// Statistics //////

    /**
     * Get the number of levels in the tree.
     */
    unsigned int statTreeHeight() { return height; }

    /**
     * Get the arity of the tree.
     */
    unsigned int statTreeArity() { return arity; }

    /**
     * Get the size of a single hash, in bytes.
     */
    unsigned int statOutputHashSize() { return hashOutputSize; }

    /**
     * Get the total amount of data that is protected by the tree, in bytes.
     */
    long long statDataProtected();

    /**
     * Get the total amount of data in the data structure, in bytes.
     */
    long long statStructureSize();

    /**
     * Get the size of the full data structure relative to the amount of data
     * that is being protected.
     *
     * This calculates the data structure size *relative* to the protected data
     * size. So you could use the output of this function to say "You need X%
     * additional data to be able to protect this data."
     */
    double statDataOverheadRatio();
};

} // namespace gem5

#endif // __MEM_MTREE_TREE_HH__
