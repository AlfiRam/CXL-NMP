#include "mem/mtree/abstract_tree.hh"

namespace gem5
{

const char* AbstractIntegrityTree::treeNodeStrings[TREE_NODE_TYPE_COUNT] = {
  "TreeNode",
  "Counter",
  "MAC"
};

} // namespace gem5
