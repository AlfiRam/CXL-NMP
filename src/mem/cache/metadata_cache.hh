#ifndef __MEM_CACHE_METADATA_CACHE_HH__
#define __MEM_CACHE_METADATA_CACHE_HH__

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mem/mtree/abstract_tree.hh"
#include "mem/mtree/timing_tree.hh"

namespace gem5
{

/**
 * Top-level interface for a metadata cache. This specifies the structure of
 * cache entries and the API that other components can interact with the cache.
 */
class AbstractMetadataCache
{
  public:
    typedef uint64_t EntryKey;
    typedef struct
    {
      bool dirty;
      bool pending_eviction;
      bool locked;
      /**
       * Relative counter for what the least-recently used item is.
       *
       * 0 is most recent, max value is least recent.
       */
      unsigned short last_used;
    } EntryValue;

    enum ReplacementPolicy
    {
      /// The basic tree node.
      Random = 0,
      LRU = 1
    };

    virtual ~AbstractMetadataCache() {};

    ReplacementPolicy replacementPolicy;

    virtual std::pair<EntryKey, EntryValue> find(EntryKey new_data) = 0;

    /**
     * Mock the access of a metadata cache entry.
     *
     * @returns True if cache hit, false if cache miss.
     */
    virtual bool access(EntryKey data) = 0;

    /**
     * Insert a piece of data to the metadata cache. If there is no space,
     * nothing will be inserted.
     *
     * @returns True if the insertion was successful, false otherwise.
     */
    virtual bool insert(EntryKey new_data) = 0;

    /**
     * Simulate modification of a cache line by specifying the entry to edit.
     * This will cause the line to be marked as dirty.
     *
     * This assumes that `data` already exists in the cache.
     */
    virtual void modify(EntryKey modified_data) = 0;

    /**
     * Check for the existence of `search_data` in the metadata cache.
     */
    virtual bool contains(EntryKey search_data) = 0;

    /**
     * Check for the existence of `search_data` in the metadata cache.
     *
     * Do not panic if the data being checked is pending eviction.
     */
    virtual bool containsPendingOkay(EntryKey search_data) = 0;

    /**
     * Evict a random cache line from the metadata cache. If the selected cache
     * line is dirty, then it will be marked as 'pending eviction', and require
     * the caller to perform additional checks as needed and call
     * `finishEvict(evicted_data)`, where `evicted_data` is
     * `evict(...).first`.
     *
     * It is guaranteed that a line already pending eviction will not be
     * selected for eviction.
     *
     * @param ignored_data Data that should not be evicted.
     * @param replacement The node that is intended to replace the evicted
     *                    cache line. If no replacement is specified, `0` can
     *                    be used to indicate the replacement.
     * @return The data that was evicted. Note that if the entry returned has
     *         the 'pending eviction' flag set to true, the eviction is not
     *         complete.
     */
    virtual std::pair<EntryKey, EntryValue> evict(
      std::unordered_set<EntryKey> ignored_data, EntryKey replacement) = 0;

    virtual std::pair<EntryKey, EntryValue> evict(
      std::unordered_set<EntryKey> ignored_data) = 0;

    virtual std::pair<EntryKey, EntryValue> evict(EntryKey ignored_data) = 0;

    virtual std::pair<EntryKey, EntryValue> evict() = 0;

    /**
     * Finish an eviction as followed from `evict()`.
     */
    virtual void finishEvict(EntryKey evicted_data) = 0;

    /**
     * Lock a node to prevent it from being evicted.
     */
    virtual void lock(EntryKey data) = 0;

    /**
     * Lock a node, but do not panic if this is called on an already-locked
     * node.
     */
    virtual void lockDupeOkay(EntryKey data) = 0;

    /**
     * Unlock a node to allow it to be evicted.
     */
    virtual void unlock(EntryKey data) = 0;

    /**
     * Unlock a node, but do not panic if this is called on an already-unlocked
     * node.
     */
    virtual void unlockDupeOkay(EntryKey data) = 0;

    virtual size_t getSize() = 0;

    virtual unsigned int getDirtyLineCount() = 0;

    virtual unsigned int getPendingEvictionCount() = 0;

    virtual unsigned int getLockedLineCount() = 0;

    /**
     * Return the lowest cached entry that is an ancestor of `data`. If no
     * entry exists, `data` is returned back.
     */
    virtual EntryKey getLowestCachedAncestor(EntryKey data) = 0;
};


/**
 * A basic unified metadata cache.
 *
 * A MetadataCache is composed of one or more `CacheSet`s, where the size of
 * each CacheSet is based on the declared associativity of the MetadataCache.
 */
class MetadataCache : public AbstractMetadataCache
{
  class CacheSet
  {
    private:
      // Add defined entry type

      typedef AbstractMetadataCache::EntryKey EntryKey;
      typedef AbstractMetadataCache::EntryValue EntryValue;
      typedef AbstractMetadataCache::ReplacementPolicy ReplacementPolicy;

      /**
       * Unique identifier for this cache set within a metadata cache.
       */
      unsigned int _id;

      unsigned int ways;

      /**
       * Internal data structure.
       */
      std::unordered_map<EntryKey, EntryValue> _data;

      /**
       * Internal count for the number of dirty cache lines in this set.
       */
      unsigned int dirty_lines;

      /**
       * Internal count for the number of cache lines in this set that
       * are currently pending eviction. These lines can be considered
       * inaccessible until they are properly evicted.
       */
      unsigned int lines_pending_eviction;

      /**
       * Internal count for the number of cache lines that are locked and
       * cannot be evicted. This is the case if a cache line is depended on by
       * another cache line that is pending insertion.
       */
      unsigned int locked_lines;

      /**
       * Metadata cache that owns this cache set.
       */
      MetadataCache *_parent;

      /**
       * Reference to integrity tree. Can be helpful for finding certain
       * relationships between nodes.
       */
      AbstractIntegrityTree *_tree;

      ReplacementPolicy replacementPolicy;

    public:
      CacheSet(
        unsigned int id,
        unsigned int ways,
        MetadataCache *parent,
        ReplacementPolicy rp = ReplacementPolicy::Random
      );

      CacheSet(
        unsigned int id,
        unsigned int ways,
        AbstractIntegrityTree *tree,
        MetadataCache *parent,
        ReplacementPolicy rp = ReplacementPolicy::Random
      );

      ~CacheSet();

      /**
       * Insert a piece of data to this set. If there is no space, nothing
       * will be inserted.
       *
       * @returns True if the insertion was successful, false otherwise.
       */
      bool insert(EntryKey new_data);

      std::pair<EntryKey, EntryValue> find(EntryKey new_data);

      /**
       * Mock the access of a cache entry in this set.
       *
       * @returns True if cache hit, false if cache miss.
       */
      bool access(EntryKey data);

      /**
       * Check for the existence of `search_data` in this cache set.
       */
      bool contains(EntryKey search_data);

      /**
       * Check for the existence of `search_data` in this set.
       *
       * Do not panic if the data being checked is pending eviction.
       */
      bool containsPendingOkay(EntryKey search_data);

      /**
       * Simulate modification of a cache line by specifying the entry to edit.
       * This will cause the line to be marked as dirty.
       *
       * This assumes that `data` already exists in this set.
       */
      void modify(EntryKey modified_data);

      /**
       * Lock a node to prevent it from being evicted.
       */
      void lock(EntryKey data);

      /**
       * Lock a node, but do not panic if this is called on an already-locked
       * node.
       */
      void lockDupeOkay(EntryKey data);

      /**
       * Unlock a node to allow it to be evicted.
       */
      void unlock(EntryKey data);

      /**
       * Unlock a node, but do not panic if this is called on an
       * already-unlocked node.
       */
      void unlockDupeOkay(EntryKey data);

      /**
       * Return the lowest cached entry that is an ancestor of `data`. If no
       * entry exists, `data` is returned back.
       */
      EntryKey getLowestCachedAncestor(EntryKey data);

      size_t getSize();

      unsigned int getDirtyLineCount();

      unsigned int getPendingEvictionCount();

      unsigned int getLockedLineCount();

      unsigned int getEvictableCount();

      unsigned int getEvictableCount(
        std::unordered_set<EntryKey> ignored_data
      );

      unsigned int getEvictableCount(
        std::unordered_set<EntryKey> ignored_data,
        EntryKey replacement
      );

      std::vector<std::pair<EntryKey, EntryValue>> getEvictable(
        std::unordered_set<EntryKey> ignored_data,
        EntryKey replacement,
        bool debug = false
      );

      std::string printLockedLines();

      bool isFull();

      bool evictionCausesCircularDependencyWithIgnoredData(
        std::unordered_set<EntryKey> ignored_data,
        EntryKey potential_victim
      );

      bool evictionCausesCircularDependencyWithIgnoredData(
        EntryKey ignored_data,
        EntryKey potential_victim
      );

      /**
       * Evict a random cache line from this set. If the selected cache line
       * is dirty, then it will be marked as 'pending eviction', and require
       * the caller to perform additional checks as needed and call
       * `finishEvict(evicted_data)`, where `evicted_data` is
       * `evict(...).first`.
       *
       * It is guaranteed that a line already pending eviction will not be
       * selected for eviction.
       *
       * @param ignored_data Data that should not be evicted.
       * @param replacement The node that is intended to replace the evicted
       *                    cache line. If no replacement is specified, `0` can
       *                    be used to indicate the replacement.
       * @return The data that was evicted. Note that if the entry returned has
       *         the 'pending eviction' flag set to true, the eviction is not
       *         complete.
       */
      std::pair<EntryKey, EntryValue> evict(
        std::unordered_set<EntryKey> ignored_data, EntryKey replacement);

      std::pair<EntryKey, EntryValue> evict(
        std::unordered_set<EntryKey> ignored_data);

      std::pair<EntryKey, EntryValue> evict(EntryKey ignored_data);

      std::pair<EntryKey, EntryValue> evict();

      /**
       * Finish an eviction as followed from `evict()`.
       */
      void finishEvict(EntryKey evicted_data);
  };

  private:
    unsigned int _associativity;
    unsigned int set_count;

    /**
     * Size of how much data can be stored in total (in bytes).
     */
    unsigned int capacity;

    /**
     * Individual sets being stored.
     */
    std::vector<CacheSet*> _sets;

  protected:
    /**
     * Reference to integrity tree. Can be helpful for finding certain
     * relationships between nodes.
     */
    AbstractIntegrityTree *_tree;

  public:
    // Constructor based on sets and associativity
    MetadataCache(
      unsigned int set_count,
      unsigned int associativity,
      ReplacementPolicy rp = ReplacementPolicy::Random
    );

    // Constructor based on total size and associativity
    MetadataCache(
      size_t total_entries,
      unsigned int associativity,
      AbstractIntegrityTree *tree,
      ReplacementPolicy rp = ReplacementPolicy::Random
    );

    ~MetadataCache();

    // void printInitDetails();

    /**
     * Return the cache set that is associated with this entry.
     *
     * There should only ever be one possible return value for each entry.
     */
    virtual size_t selectCacheSet(EntryKey data);

    /**
     * Insert a piece of data to the metadata cache. If there is no space,
     * nothing will be inserted.
     *
     * @returns True if the insertion was successful, false otherwise.
     */
    bool insert(EntryKey new_data) override;

    std::pair<EntryKey, EntryValue> find(EntryKey new_data) override;

    /**
     * Mock the access of a metadata cache entry.
     *
     * @returns True if cache hit, false if cache miss.
     */
    bool access(EntryKey data) override;

    /**
     * Check for the existence of `search_data` in the metadata cache.
     */
    bool contains(EntryKey search_data) override;

    /**
     * Check for the existence of `search_data` in the metadata cache.
     *
     * Do not panic if the data being checked is pending eviction.
     */
    bool containsPendingOkay(EntryKey search_data) override;

    /**
     * Simulate modification of a cache line by specifying the entry to edit.
     * This will cause the line to be marked as dirty.
     *
     * This assumes that `data` already exists in the cache.
     */
    void modify(EntryKey modified_data) override;

    /**
     * Lock a node to prevent it from being evicted.
     */
    void lock(EntryKey data) override;

    /**
     * Lock a node, but do not panic if this is called on an already-locked
     * node.
     */
    void lockDupeOkay(EntryKey data) override;

    /**
     * Unlock a node to allow it to be evicted.
     */
    void unlock(EntryKey data) override;

    /**
     * Unlock a node, but do not panic if this is called on an already-unlocked
     * node.
     */
    void unlockDupeOkay(EntryKey data) override;

    /**
     * Return the lowest cached entry that is an ancestor of `data`. If no
     * entry exists, `data` is returned back.
     */
    EntryKey getLowestCachedAncestor(EntryKey data) override;

    size_t getSize() override;

    unsigned int getDirtyLineCount() override;

    unsigned int getPendingEvictionCount() override;

    unsigned int getLockedLineCount() override;

    std::string printLockedLines();

    bool isFull();

    /**
     * Evict a random cache line from the metadata cache. If the selected cache
     * line is dirty, then it will be marked as 'pending eviction', and require
     * the caller to perform additional checks as needed and call
     * `finishEvict(evicted_data)`, where `evicted_data` is
     * `evict(...).first`.
     *
     * It is guaranteed that a line already pending eviction will not be
     * selected for eviction.
     *
     * @param ignored_data Data that should not be evicted.
     * @param replacement The node that is intended to replace the evicted
     *                    cache line. If no replacement is specified, `0` can
     *                    be used to indicate the replacement.
     * @return The data that was evicted. Note that if the entry returned has
     *         the 'pending eviction' flag set to true, the eviction is not
     *         complete.
     */
    std::pair<EntryKey, EntryValue> evict(
      std::unordered_set<EntryKey> ignored_data,
      EntryKey replacement
    ) override;

    std::pair<EntryKey, EntryValue> evict(
      std::unordered_set<EntryKey> ignored_data) override;

    std::pair<EntryKey, EntryValue> evict(EntryKey ignored_data) override;

    std::pair<EntryKey, EntryValue> evict() override;

    /**
     * Finish an eviction as followed from `evict()`.
     */
    void finishEvict(EntryKey evicted_data) override;
};


class PartitionedMetadataCache : public MetadataCache
{
  public:
    typedef AbstractIntegrityTree::TreeNodeType TreeNodeType;

    // Constructor based on total size and associativity
    PartitionedMetadataCache(
      size_t tree_entries,
      size_t counter_entries,
      size_t mac_entries,
      unsigned int associativity,
      AbstractIntegrityTree *tree,
      ReplacementPolicy rp = ReplacementPolicy::Random
    );

    ~PartitionedMetadataCache();

    /**
     * Return the cache set that is associated with this entry.
     *
     * There should only ever be one possible return value for each entry.
     */
    size_t selectCacheSet(EntryKey data) override;

  private:
    /**
     * Counts of how many of each type of cache set there are for this metadata
     * cache, for each type of tree node.
     */
    std::unordered_map<TreeNodeType, size_t> setTypeCounts;

    /**
     * The first cache set index of each type.
     */
    std::unordered_map<TreeNodeType, size_t> setTypeFirstIndex;
};

} // namespace gem5

#endif // __MEM_CACHE_METADATA_CACHE_HH__
