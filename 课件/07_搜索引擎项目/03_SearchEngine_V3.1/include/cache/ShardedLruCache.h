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
    // steady_clock 是单调时钟，不受系统时间被手动调整影响，更适合判断 TTL 过期。
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Entry {
        // 缓存 key。list 节点中保存 key，淘汰时可以同步从哈希表删除。
        std::string key;

        // 缓存值，本项目中主要是 JSON 字符串。
        std::string value;

        // 过期时间点。neverExpire 为 true 时该字段不参与判断。
        TimePoint expireAt;

        // ttlSeconds <= 0 时置为 true，表示该项只会因为容量淘汰，不会因为时间过期。
        bool neverExpire = false;
    };

    struct Shard {
        explicit Shard(std::size_t cap) : capacity(cap) {}

        // 当前分片可容纳的最大条目数。
        std::size_t capacity;

        // 保护 entries 和 index。std::list 移动节点、unordered_map 增删都不是线程安全的。
        std::mutex mutex;

        // LRU 链表：头部是最近访问，尾部是最久未访问。
        std::list<Entry> entries;

        // key -> list 迭代器。哈希表提供 O(1) 定位，链表提供 O(1) 移动和淘汰。
        std::unordered_map<std::string, std::list<Entry>::iterator> index;
    };

private:
    /**
     * @brief 根据 key 的哈希值选择所属分片。
     */
    Shard& shard_for(const std::string& key);

    /**
     * @brief 判断缓存项在 now 时刻是否已经过期。
     */
    static bool expired(const Entry& entry, TimePoint now);

    /**
     * @brief 根据 TTL 生成过期时间，并通过 neverExpire 输出是否永不过期。
     */
    static TimePoint make_expire_time(int ttlSeconds, bool& neverExpire);

    /**
     * @brief 在已经持有分片锁的前提下清理过期项。
     */
    static void prune_expired_locked(Shard& shard, TimePoint now);

    /**
     * @brief 在已经持有分片锁的前提下按 LRU 淘汰到容量以内。
     */
    static void evict_to_capacity_locked(Shard& shard);

private:
    // 缓存总容量，构造时会按分片数拆分给各个 Shard。
    std::size_t capacity_;

    // std::hash<std::string> 用于把 key 映射到分片下标。
    std::hash<std::string> hash_;

    // unique_ptr 保存 Shard，避免 vector 扩容/移动时尝试移动包含 mutex 的对象。
    std::vector<std::unique_ptr<Shard>> shards_;
};
