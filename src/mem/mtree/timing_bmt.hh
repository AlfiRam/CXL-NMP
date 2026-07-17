#ifndef __MEM_MTREE_TIMING_BMT_HH__
#define __MEM_MTREE_TIMING_BMT_HH__

#include <cstdint>

#include "mem/mtree/abstract_tree.hh"

namespace gem5 {

/**
 * Implementation of a Bonsai Merkle Tree. Currently assumes that the tree has
 * nodes that are all the same size. Additionally, there is a direct linear
 * mapping between nodes and their children/parent nodes.
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
 *        .... (Cache Line -- Counter Data) (Cache Line) ... (Cache Line) ....
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
 *
 * In addition to just the tree structure, there are also additional Blocks for
 * MAC and counter data.
 *
 * It is expected that for each cache line of real data, there is a counter and
 * MAC associated with it. This uses a split counter approach, where there is
 * a major counter for each page, and a minor counter for each cache line
 * within a page. One set of major and minor counters for a page should be
 * contained within a single counter block.
 *
 * Full arrangement:
 *
 * ```
 * Node index:
 *  0                                                        dataSize - 1
 * [                    |                  |                             ]
 * [     BMT Nodes      |     Counters     |            MACs             ]
 * [                    |                  |                             ]
 * ```
 */
class TimingBmt : public AbstractIntegrityTree
{
  public:
    TimingBmt(unsigned int arity, uint64_t total_data);

    ~TimingBmt();

    /**
     * Cache block size. The size of the input for hashing should likely be the
     * same as the cache line size.
     */
    const unsigned int BLOCK_SIZE_BYTES = 64;

    /**
     * Page size.
     */
    const unsigned int PAGE_SIZE_BYTES = 4096;

    /**
     * Internal tracking of how many nodes are stored in total.
     */
    size_t dataSize;

    /**
     * Number of MACs used.
     */
    size_t macCount;

    /**
     * Number of MAC nodes.
     */
    size_t macNodes;

    /**
     * Number of counter nodes.
     */
    size_t counterNodes;

    /**
     * Number of tree node (used to protect counters).
     */
    size_t treeNodes;

    /**
     * How many levels of the tree there are.
     *
     * This only applies to the tree that protects counters.
     */
    unsigned int height;

    /**
     * How many children for each node in the tree for counters.
     */
    unsigned int arity;

    /**
     * Number of leaf nodes in the tree for counters.
     */
    size_t leaves;

    /**
     * The size (in bytes) of how much data is input into the hashing function
     * (producing MACs and tree nodes).
     */
    unsigned int hashInputSize;

    /**
     * The size (in bytes) of how much data is output from the hashing
     * function (producing MACs and tree nodes).
     */
    unsigned int hashOutputSize;

    /**
     * Number of bits used for each major counter.
     */
    unsigned int majorCounterBits = 64;

    /**
     * Number of bits used for each minor counter.
     */
    unsigned int minorCounterBits;

    /**
     * Returns the index of the node, relative to its type.
     *
     * For example, if there are 5 MAC nodes, and the index is `5`, `0` is
     * returned since it is the first node of the next "segment" of nodes in
     * the arrangement.
     */
    size_t indexOfType(size_t index);

    /**
     * Returns the index of the first node for that type.
     */
    size_t firstOfType(TreeNodeType type);

    ////// API //////

    void processWrite(
      size_t address,
      std::vector<unsigned char> newData
    ) override {
    }

    bool isMocking() override { return true; };

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


    ////// Statistics //////

    /**
     * Get the number of levels in the tree.
     */
    unsigned int statTreeHeight() override;

    /**
     * Get the arity of the tree.
     */
    unsigned int statTreeArity() override;

    /**
     * Get the size of a single hash, in bytes.
     */
    unsigned int statOutputHashSize() override;

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

#endif // __MEM_MTREE_TIMING_BMT_HH__
