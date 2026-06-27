#pragma once

#include "Cache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 线程安全的分片简化 W-TinyLFU 本地缓存。
 *
 * 每个分片由三段访问顺序链表组成：
 *
 * 1. Window LRU：接收所有新数据，保留最近出现的新热点；
 * 2. Main Probation：保存通过频率准入的候选数据；
 * 3. Main Protected：保存至少在 Main 中再次命中的稳定热点。
 *
 * Window 淘汰出的 candidate 只有在 Main 未满，或者访问频率严格高于 Main
 * Probation 尾部 victim 时，才会进入 Main。这样可阻止大量只访问一次的查询持续
 * 冲掉稳定热点。
 *
 * 当前阶段使用“精确频率哈希表 + 周期减半”实现频率估计，便于理解和验证。
 * 下一阶段可以把该部分替换成 Count-Min Sketch + Doorkeeper，而 Window、SLRU、
 * 分片锁和准入流程均可继续复用。
 */
class ShardedWTinyLfuCache : public Cache
{
public:
    /**
     * @brief 缓存运行统计的只读快照。
     */
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t admissions = 0;
        std::uint64_t rejections = 0;
        std::uint64_t evictions = 0;
        std::uint64_t expirations = 0;
    };

    /**
     * @param capacity 缓存总条目数，至少为 1。
     * @param shardCount 分片数量，至少为 1 且不会超过总容量。
     * @param windowPercent Window 占每个分片容量的百分比，合法范围为 [1, 99]。
     * @param protectedPercent Main Protected 占 Main 容量上限的百分比，范围 [1, 99]。
     * @param frequencySampleMultiplier 频率采样上限相对容量的倍数，达到上限后衰减。
     */
    ShardedWTinyLfuCache(std::size_t capacity,
                         std::size_t shardCount,
                         std::size_t windowPercent = 1,
                         std::size_t protectedPercent = 80,
                         std::size_t frequencySampleMultiplier = 10);
    ~ShardedWTinyLfuCache() override;

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

    std::size_t capacity() const { return capacity_; }
    std::size_t shard_count() const { return shards_.size(); }
    std::size_t window_percent() const { return windowPercent_; }
    std::size_t protected_percent() const { return protectedPercent_; }
    Stats stats() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class Segment {
        Window,
        Probation,
        Protected
    };

    struct Entry {
        std::string key;
        std::string value;
        TimePoint expireAt;
        bool neverExpire = false;
    };

    using EntryList = std::list<Entry>;

    struct Location {
        Segment segment;
        EntryList::iterator iterator;
    };

    struct Shard {
        Shard(std::size_t cap,
              std::size_t windowCap,
              std::size_t protectedCap,
              std::size_t sampleSize)
            : capacity(cap),
              windowCapacity(windowCap),
              mainCapacity(cap - windowCap),
              protectedCapacity(protectedCap),
              frequencySampleSize(sampleSize)
        {}

        std::size_t capacity;
        std::size_t windowCapacity;
        std::size_t mainCapacity;
        std::size_t protectedCapacity;
        std::size_t frequencySampleSize;
        std::size_t frequencySamples = 0;

        std::mutex mutex;
        EntryList window;
        EntryList probation;
        EntryList protectedEntries;
        std::unordered_map<std::string, Location> index;

        // 第九阶段使用精确计数；第十阶段将替换为有界空间的概率型频率估计器。
        std::unordered_map<std::string, std::uint32_t> frequencies;
    };

private:
    Shard& shard_for(const std::string& key);

    static bool expired(const Entry& entry, TimePoint now);
    static TimePoint make_expire_time(int ttlSeconds, bool& neverExpire);
    static EntryList& list_for(Shard& shard, Segment segment);

    void record_frequency_locked(Shard& shard, const std::string& key);
    static std::uint32_t frequency_locked(const Shard& shard, const std::string& key);
    void touch_locked(Shard& shard,
                      std::unordered_map<std::string, Location>::iterator indexIt);
    void remove_locked(Shard& shard,
                       std::unordered_map<std::string, Location>::iterator indexIt,
                       bool eviction,
                       bool expiration);
    void prune_expired_locked(Shard& shard, TimePoint now);
    void drain_window_locked(Shard& shard);
    void admit_to_main_locked(Shard& shard, Entry candidate);

private:
    std::size_t capacity_;
    std::size_t windowPercent_;
    std::size_t protectedPercent_;
    std::size_t frequencySampleMultiplier_;
    std::hash<std::string> hash_;
    std::vector<std::unique_ptr<Shard>> shards_;

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> admissions_{0};
    std::atomic<std::uint64_t> rejections_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> expirations_{0};
};
