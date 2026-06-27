#pragma once

#include "Cache.h"
#include "TinyLfuFrequencySketch.h"

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
 * @brief 线程安全的分片 W-TinyLFU 本地缓存。
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
 * 频率估计由固定空间的 Count-Min Sketch、Bloom Doorkeeper 和周期老化组成。
 * Doorkeeper 过滤首次访问，Count-Min Sketch 用 4-bit 饱和计数器近似统计重复
 * 访问，Frequency Aging 定期将历史频率减半，避免旧热点永久占据 Main。
 */
class ShardedWTinyLfuCache : public Cache
{
public:
    /**
     * @brief 缓存运行统计的只读快照。
     */
    struct Stats {
        // get() 命中且缓存项未过期的次数。
        std::uint64_t hits = 0;
        // key 不存在或缓存项已过期的次数。
        std::uint64_t misses = 0;
        // Window candidate 成功进入 Main Probation 的次数。
        std::uint64_t admissions = 0;
        // candidate 因频率不够或 Main 不可用而被拒绝的次数。
        std::uint64_t rejections = 0;
        // 因容量限制删除的 victim，以及被拒绝丢弃的 candidate 总数。
        std::uint64_t evictions = 0;
        // get()/put() 惰性清理掉的 TTL 过期项数量。
        std::uint64_t expirations = 0;
        // 所有分片累计触发 Frequency Aging 的次数。
        std::uint64_t frequencyAges = 0;
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

    /** @brief 查询缓存；命中时更新所属分段的访问顺序。 */
    bool get(const std::string& key, std::string& value) override;

    /** @brief 写入或更新缓存；新 key 总是先进入 Window。 */
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;

    /** @brief 从缓存值结构中删除 key；概率型频率历史由 Aging 自然衰减。 */
    void erase(const std::string& key) override;

    /** @brief 返回整个缓存配置的最大条目数。 */
    std::size_t capacity() const { return capacity_; }
    /** @brief 返回实际创建的分片数量。 */
    std::size_t shard_count() const { return shards_.size(); }
    /** @brief 返回构造时归一化后的 Window 容量百分比。 */
    std::size_t window_percent() const { return windowPercent_; }
    /** @brief 返回构造时归一化后的 Main Protected 容量百分比。 */
    std::size_t protected_percent() const { return protectedPercent_; }
    /** @brief 返回所有分片频率估计器预分配存储区的总字节数。 */
    std::size_t frequency_sketch_bytes() const { return frequencySketchBytes_; }
    /** @brief 原子读取各项累计计数并返回一个互相独立的统计快照。 */
    Stats stats() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // enum class 不会把 Window 等枚举名泄漏到外层作用域，也不会隐式转成整数。
    enum class Segment {
        Window,
        Probation,
        Protected
    };

    struct Entry {
        // 缓存 key；淘汰链表节点时用它同步删除 index 中的映射。
        std::string key;
        // 缓存 value；当前主要保存 JSON、文档序列化结果或摘要字符串。
        std::string value;
        // 使用 steady_clock 表示的绝对过期时刻；neverExpire=true 时不参与判断。
        TimePoint expireAt;
        // ttlSeconds <= 0 时为 true，表示只接受容量淘汰，不因 TTL 失效。
        bool neverExpire = false;
    };

    // 三个分段使用相同节点类型，因此统一定义链表别名，Location 可复用迭代器类型。
    using EntryList = std::list<Entry>;

    struct Location {
        // 节点当前所在分段。跨链表 splice 后必须同步更新该字段。
        Segment segment;
        // 指向对应链表节点；list::splice 不会使被移动节点的迭代器失效。
        EntryList::iterator iterator;
    };

    struct Shard {
        /**
         * @brief 构造一个独立分片并确定各分段容量。
         *
         * mainCapacity 直接由 cap-windowCap 得出；frequencySketch 使用整个分片
         * 容量估算固定数组大小，并使用 sampleSize 作为老化周期。
         */
        Shard(std::size_t cap,
              std::size_t windowCap,
              std::size_t protectedCap,
              std::size_t sampleSize)
            : capacity(cap),
              windowCapacity(windowCap),
              mainCapacity(cap - windowCap),
              protectedCapacity(protectedCap),
              frequencySketch(cap, sampleSize)
        {}

        // 当前分片允许保存的总条目数。
        std::size_t capacity;
        // Window LRU 的容量上限。
        std::size_t windowCapacity;
        // Main 总容量，等于 capacity - windowCapacity。
        std::size_t mainCapacity;
        // Main Protected 的容量上限，剩余 Main 空间由 Probation 使用。
        std::size_t protectedCapacity;

        // 同时保护三条链表、index 和 frequencySketch；同一分片内操作串行化。
        std::mutex mutex;
        // Window/Probation/Protected 均以头部表示 MRU、尾部表示 LRU。
        EntryList window;
        EntryList probation;
        EntryList protectedEntries;
        // key -> 所属分段和链表迭代器，提供平均 O(1) 的节点定位。
        std::unordered_map<std::string, Location> index;

        // 所有调用均发生在 mutex 保护范围内，因此估计器本身不需要重复加锁。
        TinyLfuFrequencySketch frequencySketch;
    };

private:
    /** @brief 根据 key 哈希选择分片；相同 key 始终落到同一分片。 */
    Shard& shard_for(const std::string& key);

    /** @brief 判断一个 Entry 在 now 时刻是否已过期。 */
    static bool expired(const Entry& entry, TimePoint now);
    /** @brief 将相对 TTL 转成绝对时间，并输出是否永不过期。 */
    static TimePoint make_expire_time(int ttlSeconds, bool& neverExpire);
    /** @brief 根据 Segment 返回对应链表，集中处理三段选择逻辑。 */
    static EntryList& list_for(Shard& shard, Segment segment);

    // 以下函数名带 locked，表示调用者必须已经持有 shard.mutex，函数内部不重复加锁。
    /** @brief 记录一次 key 访问，并累计可能触发的 Frequency Aging。 */
    void record_frequency_locked(Shard& shard, const std::string& key);
    /** @brief 查询 key 在当前分片频率估计器中的近似频率。 */
    std::uint32_t frequency_locked(const Shard& shard, const std::string& key) const;
    /** @brief 处理命中后的 LRU 移动、Probation 晋升和 Protected 降级。 */
    void touch_locked(Shard& shard,
                      std::unordered_map<std::string, Location>::iterator indexIt);
    /** @brief 同时删除链表节点与索引，并按删除原因更新统计。 */
    void remove_locked(Shard& shard,
                       std::unordered_map<std::string, Location>::iterator indexIt,
                       bool eviction,
                       bool expiration);
    /** @brief 遍历当前分片的三个分段并删除 TTL 过期项。 */
    void prune_expired_locked(Shard& shard, TimePoint now);
    /** @brief Window 超限时依次取出尾部 candidate 并执行 Main 准入。 */
    void drain_window_locked(Shard& shard);
    /** @brief 比较 candidate/victim 频率并决定接纳、替换或拒绝。 */
    void admit_to_main_locked(Shard& shard, Entry candidate);

private:
    // 整个缓存的容量和策略配置；均在构造后保持只读。
    std::size_t capacity_;
    std::size_t windowPercent_;
    std::size_t protectedPercent_;
    std::size_t frequencySampleMultiplier_;
    // 构造各分片时累计得到，用于展示固定空间频率估计器的额外内存开销。
    std::size_t frequencySketchBytes_ = 0;
    // std::hash<std::string> 同时用于选择分片和生成频率估计器输入。
    std::hash<std::string> hash_;
    // Shard 含 mutex，不能安全复制/移动；unique_ptr 让 vector 只移动指针。
    std::vector<std::unique_ptr<Shard>> shards_;

    // 统计跨多个分片更新，使用 atomic 避免再引入一把全局统计锁。
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> admissions_{0};
    std::atomic<std::uint64_t> rejections_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> expirations_{0};
    std::atomic<std::uint64_t> frequencyAges_{0};
};
