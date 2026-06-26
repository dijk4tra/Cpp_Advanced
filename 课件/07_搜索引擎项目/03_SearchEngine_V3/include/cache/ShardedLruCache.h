#pragma once

#include "Cache.h"

#include <chrono>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 线程安全的分片 LRU 缓存。
 *
 * 总容量按分片数量平均拆分。每个分片都有独立的互斥锁、哈希表和 LRU 链表，避免
 * 所有工作线程争抢同一把锁。get 命中时会把节点移动到链表头部，因此 get 也是写
 * LRU 状态的操作，需要持有对应分片锁。
 */
class ShardedLruCache : public Cache
{
public:
    /**
     * @param capacity 缓存总容量，至少为 1。
     * @param shardCount 分片数量，至少为 1。
     */
    ShardedLruCache(std::size_t capacity, std::size_t shardCount);
    ~ShardedLruCache() override;

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

    std::size_t capacity() const { return capacity_; }
    std::size_t shard_count() const { return shards_.size(); }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Entry {
        std::string key;
        std::string value;
        TimePoint expireAt;
        bool neverExpire = false;
    };

    struct Shard {
        explicit Shard(std::size_t cap) : capacity(cap) {}

        std::size_t capacity;
        std::mutex mutex;
        std::list<Entry> entries;
        std::unordered_map<std::string, std::list<Entry>::iterator> index;
    };

private:
    Shard& shard_for(const std::string& key);
    static bool expired(const Entry& entry, TimePoint now);
    static TimePoint make_expire_time(int ttlSeconds, bool& neverExpire);
    static void prune_expired_locked(Shard& shard, TimePoint now);
    static void evict_to_capacity_locked(Shard& shard);

private:
    std::size_t capacity_;
    std::hash<std::string> hash_;
    std::vector<std::unique_ptr<Shard>> shards_;
};
