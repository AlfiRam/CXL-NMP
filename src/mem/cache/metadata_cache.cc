#include "mem/cache/metadata_cache.hh"

#include <cstdlib>

#include "base/trace.hh"
#include "debug/MetadataCache.hh"
#include "debug/MetadataCacheEviction.hh"

namespace gem5
{
  MetadataCache::MetadataCache(
    unsigned int set_count,
    unsigned int associativity,
    ReplacementPolicy rp
  ) :
    _associativity(associativity),
    set_count(set_count)
  {
    assert(set_count > 0);

    // Defining "0" as "fully associative."
    if (associativity == 0) {
      _associativity = set_count;
    }

    assert(_associativity > 0);
    assert(set_count % _associativity == 0);

    _sets = std::vector<CacheSet*>();
    for (size_t i = 0; i < set_count; i++) {
      _sets.push_back(new CacheSet(i, _associativity, this));
    }

    replacementPolicy = rp;

    // TODO Potentially add size information back in later
  }

  MetadataCache::MetadataCache(
    size_t total_entries,
    unsigned int associativity,
    AbstractIntegrityTree *tree,
    ReplacementPolicy rp
  ) :
    _associativity(associativity), _tree(tree)
  {
    assert(total_entries > 0);

    // Defining "0" as "fully associative."
    if (associativity == 0) {
      _associativity = total_entries;
    }

    assert(_associativity > 0);
    assert(total_entries % _associativity == 0);

    set_count = total_entries / _associativity;
    assert(set_count > 0);

    _sets = std::vector<CacheSet*>();
    for (size_t i = 0; i < set_count; i++) {
      _sets.push_back(new CacheSet(i, _associativity, tree, this));
    }

    capacity = _associativity * set_count;
    assert(capacity == total_entries);

    replacementPolicy = rp;
  }


  MetadataCache::~MetadataCache()
  {
    // delete sets;
    for (auto s : _sets) {
      delete s;
    }
  }

  // TODO Needs to be rewritten.
  // void
  // MetadataCache::printInitDetails()
  // {
  //   DPRINTF(MetadataCache, "%s: Metadata cache initialized! Details:\n",
  //     __func__);
  //   // Rewrite or check all of this
  //   // DPRINTF(MetadataCache, "%s: Single entry size: %dB\n",
  //   //   __func__, BasicCacheEntry::entrySize());
  //   // DPRINTF(MetadataCache, "%s: Total metadata cache size: %dB\n",
  //   //   __func__, capacity);
  //   // DPRINTF(MetadataCache, "%s: Number of sets: %d\n",
  //   //   __func__, set_count);
  //   // DPRINTF(MetadataCache, "%s: Associativity: %d\n",
  //   //   __func__, associativity);
  // }

  MetadataCache::CacheSet::CacheSet(
    unsigned int id,
    unsigned int ways,
    MetadataCache *parent,
    ReplacementPolicy rp
  ) :
    _id(id), ways(ways), dirty_lines(0), lines_pending_eviction(0),
    locked_lines(0), _parent(parent), _tree(nullptr), replacementPolicy(rp)
  {
    srand(time(0));
  }

  MetadataCache::CacheSet::CacheSet(
    unsigned int id,
    unsigned int ways,
    AbstractIntegrityTree *tree,
    MetadataCache *parent,
    ReplacementPolicy rp
  ) : CacheSet(id, ways, parent, rp)
  {
    _tree = tree;
  }

  MetadataCache::CacheSet::~CacheSet()
  {

  }

  size_t
  MetadataCache::selectCacheSet(EntryKey data)
  {
    return data % set_count;
  }

  bool
  MetadataCache::insert(EntryKey new_data)
  {
    auto set = selectCacheSet(new_data);

    return _sets[set]->insert(new_data);
  }

  bool
  MetadataCache::CacheSet::insert(EntryKey new_data)
  {
    if (isFull()) {
      // The cache is full. Do not insert more.
      DPRINTF(MetadataCache,
        "%s: set %u: Cannot insert %lu into metadata cache. Full. "
        "(Size: %lu)\n",
        __func__, _id, new_data, getSize());
      return false;
    }

    // The data should not yet exist in the cache already.
    assert(!contains(new_data));

    EntryValue entry_value = {
      .dirty = false,
      .pending_eviction = false,
      .locked = false,
      .last_used = 0,
    };
    _data.emplace(new_data, entry_value);
    DPRINTF(MetadataCache,
      "%s: set %u: Inserted %lu into metadata cache. (New size: %lu)\n",
      __func__, _id, new_data, getSize());

    // TODO Upon insertion, increment everything else in the set in LRU policy

    return true;
  }

  std::pair<AbstractMetadataCache::EntryKey,
    AbstractMetadataCache::EntryValue>
  MetadataCache::find(EntryKey search_data)
  {
    auto set = selectCacheSet(search_data);

    return _sets[set]->find(search_data);
  }

  std::pair<MetadataCache::CacheSet::EntryKey,
    MetadataCache::CacheSet::EntryValue>
  MetadataCache::CacheSet::find(EntryKey search_data)
  {
    return *(_data.find(search_data));
  }

  bool
  MetadataCache::access(EntryKey data)
  {
    auto set = selectCacheSet(data);

    return _sets[set]->access(data);
  }

  bool
  MetadataCache::CacheSet::access(EntryKey data)
  {
    bool exists = contains(data);

    if (exists) {
      // If this is a cache hit, update the least-recently used data.

      // For all other lines, increase the usage counter.
      for (auto d : _data) {
        // Assume max value is 2**16
        if (d.second.last_used < (1 << 16)) {
          d.second.last_used += 1;
        }
      }

      // Make this entry the most recent.
      _data[data].last_used = 0;
    }

    return exists;
  }

  bool
  MetadataCache::contains(EntryKey search_data)
  {
    auto set = selectCacheSet(search_data);

    return _sets[set]->contains(search_data);
  }

  bool
  MetadataCache::CacheSet::contains(EntryKey search_data)
  {
    auto search = _data.find(search_data);
    if (search != _data.end() && search->second.pending_eviction) {
      panic("Someone is checking for a cacheline that is pending eviction.");
    }

    return (search != _data.end());
  }

  bool
  MetadataCache::containsPendingOkay(EntryKey search_data)
  {
    auto set = selectCacheSet(search_data);

    return _sets[set]->containsPendingOkay(search_data);
  }

  bool
  MetadataCache::CacheSet::containsPendingOkay(EntryKey search_data)
  {
    auto search = _data.find(search_data);
    return (search != _data.end());
  }

  void
  MetadataCache::modify(EntryKey modified_data)
  {
    auto set = selectCacheSet(modified_data);

    _sets[set]->modify(modified_data);
  }

  void
  MetadataCache::CacheSet::modify(EntryKey modified_data)
  {
    assert(contains(modified_data));
    assert(!find(modified_data).second.pending_eviction);

    if (_data[modified_data].dirty) {
      // This data is already marked dirty. Do nothing.
      DPRINTF(MetadataCache,
        "%s: set %u: Dirty cache entry %llu is modified again.\n",
        __func__, _id, modified_data);
      return;
    }

    // Mark the data item as dirty.
    _data[modified_data].dirty = true;
    dirty_lines++;

    DPRINTF(MetadataCache, "%s: set %u: Dirty lines count increased to %u\n",
      __func__, _id, dirty_lines);
  }

  void
  MetadataCache::lock(EntryKey data)
  {
    auto set = selectCacheSet(data);

    _sets[set]->lock(data);
  }

  void
  MetadataCache::CacheSet::lock(EntryKey data)
  {
    assert(contains(data));
    assert(!_data[data].locked);

    DPRINTF(MetadataCache, "%s: set %u: Locking line %llu.\n",
      __func__, _id, data);
    _data[data].locked = true;
    locked_lines++;
    DPRINTF(MetadataCache, "%s: set %u: locked_lines increased to %d\n",
      __func__, _id, locked_lines);
  }

  void
  MetadataCache::lockDupeOkay(EntryKey data)
  {
    auto set = selectCacheSet(data);

    _sets[set]->lockDupeOkay(data);
  }

  void
  MetadataCache::CacheSet::lockDupeOkay(EntryKey data)
  {
    assert(contains(data));
    DPRINTF(MetadataCache, "%s: set %u: Locking line %llu.\n",
      __func__, _id, data);

    if (_data[data].locked) {
      DPRINTF(MetadataCache, "%s: set %u: %llu already locked.\n",
        __func__, _id, data);
    } else {
      _data[data].locked = true;
      locked_lines++;
      DPRINTF(MetadataCache, "%s: set %u: locked_lines increased to %d\n",
        __func__, _id, locked_lines);
    }
  }

  void
  MetadataCache::unlock(EntryKey data)
  {
    auto set = selectCacheSet(data);

    _sets[set]->unlock(data);
  }

  void
  MetadataCache::CacheSet::unlock(EntryKey data)
  {
    assert(contains(data));
    assert(_data[data].locked);

    DPRINTF(MetadataCache, "%s: set %u: Unlocking line %llu.\n",
      __func__, _id, data);
    _data[data].locked = false;
    locked_lines--;
    DPRINTF(MetadataCache, "%s: set %u: locked_lines decreased to %d\n",
      __func__, _id, locked_lines);
  }

  void
  MetadataCache::unlockDupeOkay(EntryKey data)
  {
    auto set = selectCacheSet(data);

    _sets[set]->unlockDupeOkay(data);
  }

  void
  MetadataCache::CacheSet::unlockDupeOkay(EntryKey data)
  {
    assert(contains(data));
    DPRINTF(MetadataCache, "%s: set %u: Unlocking line %llu.\n",
      __func__, _id, data);

    if (!_data[data].locked) {
      DPRINTF(MetadataCache, "%s: set %u: %llu already unlocked.\n",
        __func__, _id, data);
    } else {
      _data[data].locked = false;
      locked_lines--;
      DPRINTF(MetadataCache, "%s: set %u: locked_lines decreased to %d\n",
        __func__, _id, locked_lines);
    }
  }

  AbstractMetadataCache::EntryKey
  MetadataCache::getLowestCachedAncestor(EntryKey data) {
    // TODO Needs more testing
    EntryKey lowest = data;
    for (auto s : _sets) {
      auto check = s->getLowestCachedAncestor(data);
      // If this is the lowest ancestor we've seen so far, save this.
      if (check != data && (check > lowest || lowest == data)) {
        lowest = check;
      }
    }

    return lowest;
  }

  MetadataCache::CacheSet::EntryKey
  MetadataCache::CacheSet::getLowestCachedAncestor(EntryKey data) {
    assert(_tree != nullptr);

    size_t index = data;
    while (index > 0) {
      index = _tree->parentBlockIndex(index);

      if (containsPendingOkay(index) && !_data[index].pending_eviction) {
        return index;
      }

      if (index == 0) {
        break;
      }
    }

    // If no such ancestor, return back `data`
    return data;
  }

  size_t
  MetadataCache::getSize()
  {
    size_t size = 0;
    for (auto set : _sets) {
      size += set->getSize();
    }

    return size;
  }

  size_t
  MetadataCache::CacheSet::getSize()
  {
    return _data.size();
  }

  unsigned int
  MetadataCache::getDirtyLineCount()
  {
    size_t count = 0;
    for (auto set : _sets) {
      count += set->getDirtyLineCount();
    }

    return count;
  }

  unsigned int MetadataCache::CacheSet::getDirtyLineCount()
  {
    return dirty_lines;
  }

  unsigned int
  MetadataCache::getPendingEvictionCount()
  {
    size_t count = 0;
    for (auto set : _sets) {
      count += set->getPendingEvictionCount();
    }

    return count;
  }

  unsigned int MetadataCache::CacheSet::getPendingEvictionCount()
  {
    return lines_pending_eviction;
  }

  unsigned int
  MetadataCache::getLockedLineCount()
  {
    size_t count = 0;
    for (auto set : _sets) {
      count += set->getLockedLineCount();
    }

    return count;
  }

  unsigned int MetadataCache::CacheSet::getLockedLineCount()
  {
    return locked_lines;
  }

  unsigned int
  MetadataCache::CacheSet::getEvictableCount()
  {
    return getEvictableCount(std::unordered_set<EntryKey>());
  }

  unsigned int
  MetadataCache::CacheSet::getEvictableCount(
    std::unordered_set<EntryKey> ignored_data)
  {
    return getEvictableCount(ignored_data, 0);
  }

  unsigned int
  MetadataCache::CacheSet::getEvictableCount(
    std::unordered_set<EntryKey> ignored_data,
    EntryKey replacement)
  {
    return getEvictable(ignored_data, replacement).size();
  }

  std::vector<std::pair<AbstractMetadataCache::EntryKey,
    AbstractMetadataCache::EntryValue>>
  MetadataCache::CacheSet::getEvictable(
    std::unordered_set<EntryKey> ignored_data,
    EntryKey replacement,
    bool debug)
  {
    if (_tree == nullptr) {
      panic("Tree is required for use of this function.");
    }

    std::vector<std::pair<EntryKey, EntryValue>> evictable;

    EntryKey replacementParent = 0;
    if (replacement != 0) {
      replacementParent = _tree->parentBlockIndex(replacement);
    }

    if (debug) {
      DPRINTF(MetadataCacheEviction,
        "%s: replacement = %llu, replacementParent = %llu\n",
        __func__, replacement, replacementParent);
    }

    for (auto it : _data) {
      // Here are all the different conditions that could cause a line to
      // not be evicted.

      if (debug) {
        DPRINTF(MetadataCacheEviction,
          "%s: -> Data item %llu [%s%s%s]\n",
          __func__, it.first,
          it.second.dirty ? "D" : " ",
          it.second.pending_eviction ? "P" : " ",
          it.second.locked ? "L" : " "
        );
      }

      // The entry should not be locked.
      if (it.second.locked) {
        if (debug) {
          DPRINTF(MetadataCacheEviction,
            "%s: This entry is locked.\n",
            __func__);
        }
        continue;
      }

      // The entry should not be pending eviction.
      if (it.second.pending_eviction) {
        if (debug) {
          DPRINTF(MetadataCacheEviction,
            "%s: This entry is pending eviction.\n",
            __func__);
        }
        continue;
      }

      // This entry should not be in the ignore list.
      if (ignored_data.find(it.first) != ignored_data.end()) {
        if (debug) {
          DPRINTF(MetadataCacheEviction,
            "%s: This entry is in the ignore list.\n",
            __func__);
        }
        continue;
      }

      // The parent of this entry is in the ignore list.
      if (ignored_data.find(_tree->parentBlockIndex(it.first)) !=
          ignored_data.end()) {
        if (debug) {
          DPRINTF(MetadataCacheEviction,
            "%s: The parent of this entry is in the ignore list.\n",
            __func__);
        }
        continue;
      }

      // Evicting this entry should not create a circular dependency for this
      // particular replacement.
      if (replacement != 0 &&
          _tree->isAncestor(replacement, it.first) &&
          _parent->getLowestCachedAncestor(it.first) == replacementParent)
      {
        if (debug) {
          DPRINTF(MetadataCacheEviction,
            "%s: Evicting this entry will cause circular dependency with "
            "replacement.\n",
            __func__);
        }
        continue;
      }

      // Evicting this entry would cause a circular dependency with once of the
      // ignored items.
      bool cont = false;
      for (auto ignored : ignored_data) {
        if (ignored != 0 && ignored != it.first &&
            evictionCausesCircularDependencyWithIgnoredData(ignored, it.first))
        {
          cont = true;
          if (debug) {
            DPRINTF(MetadataCacheEviction, "%s: Evicting this entry will "
              "cause circular dependency with one of the ignored items "
              "(%llu).\n",
              __func__, ignored);
          }
          break;
        }

      }

      if (cont) {
        continue;
      }

      if (debug) {
        DPRINTF(MetadataCacheEviction, "This entry is evictable.", __func__);
      }

      // If none of these conditions are true, we are free to potentially
      // evict this entry.
      evictable.push_back(it);
    }

    return evictable;
  }

  std::string
  MetadataCache::printLockedLines()
  {
    std::ostringstream str;

    for (size_t i = 0; i < set_count; i++) {
      ccprintf(str, "set %lu:\n", i);
      ccprintf(str, "%s", _sets[i]->printLockedLines());
    }

    return str.str();
  }

  std::string
  MetadataCache::CacheSet::printLockedLines()
  {
      std::ostringstream str;

      ccprintf(str, "locked line count: %d\n", locked_lines);

      ccprintf(str, "locked: [");
      for (auto line : _data) {
        if (line.second.locked) {
          ccprintf(str, "%llu ", line.first);
        }
      }
      ccprintf(str, "]\n");

      return str.str();
  }

  bool
  MetadataCache::isFull()
  {
    // TODO This probably won't be used, maybe delete this function
    return getSize() >= (_associativity * set_count);
  }

  bool
  MetadataCache::CacheSet::isFull()
  {
    return getSize() >= ways;
  }

  bool
  MetadataCache::CacheSet::evictionCausesCircularDependencyWithIgnoredData(
    std::unordered_set<EntryKey> ignored_data,
    EntryKey potential_victim
  )
  {
    if (_tree == nullptr) {
      panic("This function cannot be called without initializing _tree.\n");
    }

    for (auto it : ignored_data) {
      if (it == 0) {
        continue;
      }

      if (it == potential_victim) {
        return true;
      }

      bool result = evictionCausesCircularDependencyWithIgnoredData(
        it, potential_victim);
      if (result) {
        return true;
      }
    }

    return false;
  }

  bool
  MetadataCache::CacheSet::evictionCausesCircularDependencyWithIgnoredData(
    EntryKey ignored_data,
    EntryKey potential_victim
  )
  {
    if (_tree == nullptr) {
      panic("This function cannot be called without initializing _tree.\n");
    }

    EntryKey ignored_data_parent = 0;
    if (ignored_data != 0) {
      ignored_data_parent = _tree->parentBlockIndex(ignored_data);
    }

    EntryKey lowestCachedAncestor = _parent->getLowestCachedAncestor(
                              potential_victim);

    return (
      _tree->isAncestor(ignored_data, potential_victim) &&
      (
        //lowestCachedAncestor == potential_victim ||
        lowestCachedAncestor == ignored_data ||
        lowestCachedAncestor == ignored_data_parent ||
        _tree->isAncestor(lowestCachedAncestor, ignored_data))
    );
  }

  std::pair<AbstractMetadataCache::EntryKey, AbstractMetadataCache::EntryValue>
  MetadataCache::evict(
    std::unordered_set<EntryKey> ignored_data,
    EntryKey replacement)
  {
    auto set = selectCacheSet(replacement);

    return _sets[set]->evict(ignored_data, replacement);
  }

  std::pair<MetadataCache::CacheSet::EntryKey,
    MetadataCache::CacheSet::EntryValue>
  MetadataCache::CacheSet::evict(
    std::unordered_set<EntryKey> ignored_data,
    EntryKey replacement)
  {
    assert(getSize() > 0);

    auto evictable = getEvictable(ignored_data, replacement);
    if (evictable.size() == 0) {
      // Somehow, we ended up where there is no evictable data.
      // Rerun and try to find out why with debugging info.
      getEvictable(ignored_data, replacement, true);
      panic("Unable to find evictable item.");
    }
    assert(evictable.size() > 0);

    // Select the item to evict based on the replacement policy.
    size_t index;
    if (replacementPolicy == ReplacementPolicy::Random) {
      index = rand() % evictable.size();
    } else if (replacementPolicy == ReplacementPolicy::LRU) {
      // Iterate through the items and find the least recent.
      auto least_recent_value = evictable[0].second.last_used;
      auto least_recent_index = 0;

      for (size_t i = 1; i < evictable.size(); i++) {
        if (evictable[i].second.last_used > least_recent_value) {
          least_recent_value = evictable[i].second.last_used;
          least_recent_index = i;
        }
      }

      index = least_recent_index;
    } else {
      panic("Unknown replacement policy.");
    }

    EntryKey selectedEntry = evictable[index].first;

    auto iterator = _data.begin();
    while (iterator != _data.end()) {

      if (iterator->first == selectedEntry) {
        // We have found the element we are looking for.
        break;
      }

      iterator++;
    }
    assert(!iterator->second.pending_eviction);
    // The iterator is now on the element to remove.

    auto evictedData = iterator->first;
    auto dirty = iterator->second.dirty;

    DPRINTF(MetadataCache,
      "%s: set %u: %lld selected for eviction.\n",
      __func__, _id, evictedData);

    if (!dirty) {
      // If this line is clean, we can just evict this without issue.
      DPRINTF(MetadataCache,
        "%s: set %u: %lld is clean and can be evicted now.\n",
        __func__, _id, evictedData);
      _data.erase(iterator);
    } else {
      // If this line is dirty, the eviction is marked as pending, as
      // additional checks by the caller should be made.
      DPRINTF(MetadataCache,
        "%s: set %u: %lld is dirty. Marking as pending eviction.\n",
        __func__, _id, evictedData);
      iterator->second.pending_eviction = true;
      lines_pending_eviction++;
    }

    // Returns a copy of this entry in the metadata cache.
    return *iterator;
  }

  std::pair<AbstractMetadataCache::EntryKey, AbstractMetadataCache::EntryValue>
  MetadataCache::evict(std::unordered_set<EntryKey> ignored_data)
  {
    return evict(ignored_data, 0);
  }

  std::pair<MetadataCache::CacheSet::EntryKey,
    MetadataCache::CacheSet::EntryValue>
  MetadataCache::CacheSet::evict(std::unordered_set<EntryKey> ignored_data)
  {
    return evict(ignored_data, 0);
  }

  std::pair<AbstractMetadataCache::EntryKey, AbstractMetadataCache::EntryValue>
  MetadataCache::evict(EntryKey ignored_data)
  {
    std::unordered_set<EntryKey> _ignored_data;
    _ignored_data.insert(ignored_data);

    return evict(_ignored_data);
  }

  std::pair<MetadataCache::CacheSet::EntryKey,
    MetadataCache::CacheSet::EntryValue>
  MetadataCache::CacheSet::evict(EntryKey ignored_data)
  {
    std::unordered_set<EntryKey> _ignored_data;
    _ignored_data.insert(ignored_data);

    return evict(_ignored_data);
  }

  std::pair<AbstractMetadataCache::EntryKey,
    AbstractMetadataCache::EntryValue>
  MetadataCache::evict()
  {
    return evict(std::unordered_set<EntryKey>());
  }

  std::pair<MetadataCache::CacheSet::EntryKey,
    MetadataCache::CacheSet::EntryValue>
  MetadataCache::CacheSet::evict()
  {
    return evict(std::unordered_set<EntryKey>());
  }

  void
  MetadataCache::finishEvict(EntryKey evicted_data)
  {
    auto set = selectCacheSet(evicted_data);

    _sets[set]->finishEvict(evicted_data);
  }

  void
  MetadataCache::CacheSet::finishEvict(EntryKey evicted_data)
  {
    // The data we're evicting should actually be in the cache.
    assert(containsPendingOkay(evicted_data));

    auto iterator = _data.find(evicted_data);
    assert(iterator != _data.end());
    assert(iterator->second.pending_eviction);

    auto dirty = iterator->second.dirty;
    assert(dirty);

    _data.erase(iterator);
    lines_pending_eviction--;

    dirty_lines--;
    DPRINTF(MetadataCache, "%s: set %u: Dirty lines count decreased to %u\n",
      __func__, _id, dirty_lines);

    DPRINTF(MetadataCache,
      "%s: set %u: %llu evicted. Lines pending eviction: %u\n",
      __func__, _id, evicted_data, lines_pending_eviction);
  }


  PartitionedMetadataCache::PartitionedMetadataCache(
    size_t tree_entries,
    size_t counter_entries,
    size_t mac_entries,
    unsigned int associativity,
    AbstractIntegrityTree *tree,
    ReplacementPolicy rp
  ) : MetadataCache(tree_entries + counter_entries + mac_entries,
        associativity, tree, rp)
  {
    assert(tree_entries > 0);
    assert(counter_entries > 0);
    assert(mac_entries > 0);

    setTypeCounts[TreeNodeType::TreeNode] = tree_entries / associativity;
    setTypeCounts[TreeNodeType::Counter] = counter_entries / associativity;
    setTypeCounts[TreeNodeType::MAC] = mac_entries / associativity;

    setTypeFirstIndex[TreeNodeType::TreeNode] = 0;
    setTypeFirstIndex[TreeNodeType::Counter] =
      setTypeCounts[TreeNodeType::TreeNode];
    setTypeFirstIndex[TreeNodeType::MAC] =
      setTypeCounts[TreeNodeType::TreeNode] +
      setTypeCounts[TreeNodeType::Counter];
  }

  PartitionedMetadataCache::~PartitionedMetadataCache()
  {
  }

  size_t
  PartitionedMetadataCache::selectCacheSet(EntryKey data)
  {
    TreeNodeType type = _tree->getNodeType(data);

    // Index within this set type.
    size_t index = data % setTypeCounts[type];

    // Index of set among the total amount of sets.
    return setTypeFirstIndex[type] + index;
  }

} // namespace gem5
