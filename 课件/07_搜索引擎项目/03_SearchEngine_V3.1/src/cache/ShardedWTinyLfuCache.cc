#include "../../include/cache/ShardedWTinyLfuCache.h"

#include <algorithm>
#include <utility>

namespace
{
/**
 * @brief 将配置百分比限制在 [1, 99]，避免 Window 或 Protected 独占整个分片。
 *
 * 该辅助函数放在匿名命名空间中，因此只在当前源文件内可见，不会产生外部链接符号。
 */
std::size_t clamp_percent(std::size_t value)
{
    return std::max<std::size_t>(1, std::min<std::size_t>(99, value));
}
}

/**
 * @brief 构造分片 W-TinyLFU，并按总容量初始化每个分片的三段空间。
 *
 * 构造过程依次完成配置归一化、总容量拆分、Window/Main 比例计算、Protected 上限
 * 计算和频率估计器创建。构造完成后，各分片容量之和严格等于 capacity_。
 */
ShardedWTinyLfuCache::ShardedWTinyLfuCache(std::size_t capacity,
                                           std::size_t shardCount,
                                           std::size_t windowPercent,
                                           std::size_t protectedPercent,
                                           std::size_t frequencySampleMultiplier)
    : capacity_(std::max<std::size_t>(1, capacity)),
      windowPercent_(clamp_percent(windowPercent)),
      protectedPercent_(clamp_percent(protectedPercent)),
      frequencySampleMultiplier_(std::max<std::size_t>(1, frequencySampleMultiplier))
{
    // 分片数至少为 1，并且不能超过总容量。否则会出现容量为 0 的空分片，同时让
    // 实际缓存容量大于配置值。
    shardCount = std::max<std::size_t>(1, shardCount);
    shardCount = std::min(shardCount, capacity_);

    // 先给每个分片 baseCapacity 个位置，再把不能整除的 extra 个位置依次分给
    // 前 extra 个分片。例如 10/3 会拆成 4、3、3。
    const std::size_t baseCapacity = capacity_ / shardCount;
    const std::size_t extra = capacity_ % shardCount;
    // reserve 只预留 vector 存储空间，不创建 Shard；可避免循环 push_back 时扩容。
    shards_.reserve(shardCount);

    for (std::size_t i = 0; i < shardCount; ++i) {
        const std::size_t shardCapacity = baseCapacity + (i < extra ? 1 : 0);

        // Window 至少保留一个位置。分片容量为 1 时全部作为 Window；否则最多
        // 占 capacity - 1，确保 Main 至少有一个位置可执行频率准入。
        std::size_t windowCapacity =
            std::max<std::size_t>(1, shardCapacity * windowPercent_ / 100);
        if (shardCapacity > 1) {
            windowCapacity = std::min(windowCapacity, shardCapacity - 1);
        }

        const std::size_t mainCapacity = shardCapacity - windowCapacity;
        std::size_t protectedCapacity = mainCapacity * protectedPercent_ / 100;
        if (mainCapacity > 1) {
            protectedCapacity = std::max<std::size_t>(1, protectedCapacity);
            // Main 必须至少留一个 Probation victim，才能执行 TinyLFU 准入比较。
            protectedCapacity = std::min(protectedCapacity, mainCapacity - 1);
        } else {
            protectedCapacity = 0;
        }

        const std::size_t sampleSize =
            std::max<std::size_t>(1, shardCapacity * frequencySampleMultiplier_);
        // make_unique 在堆上构造含 mutex 的 Shard。unique_ptr 可以移动，而 mutex
        // 不可复制/移动，因此适合存入 vector。
        auto shard = std::make_unique<Shard>(shardCapacity,
                                             windowCapacity,
                                             protectedCapacity,
                                             sampleSize);
        // 在移动 unique_ptr 前读取估计器大小；std::move 后 shard 将变成空指针。
        frequencySketchBytes_ += shard->frequencySketch.memory_usage_bytes();
        shards_.push_back(std::move(shard));
    }
}

ShardedWTinyLfuCache::~ShardedWTinyLfuCache() = default;

/**
 * @brief 查询缓存，并按 W-TinyLFU 规则更新频率和访问顺序。
 *
 * 查询流程：选择分片 -> 加分片锁 -> 记录访问频率 -> 查索引 -> 检查 TTL ->
 * 更新分段顺序 -> 返回 value。get() 会修改链表和频率，因此不是只读操作。
 */
bool ShardedWTinyLfuCache::get(const std::string& key, std::string& value)
{
    // shard_for 返回引用，不复制包含 mutex 的 Shard。lock_guard 使用 RAII：构造时
    // 加锁，函数任意路径 return 时都自动解锁。
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    // TinyLFU 必须同时观察命中和未命中访问。只统计已经缓存的 key 会使新热点
    // 永远没有足够频率通过准入比较。
    record_frequency_locked(shard, key);

    // unordered_map 平均 O(1) 定位。auto 推导出的类型是 index 的 iterator。
    auto indexIt = shard.index.find(key);
    if (indexIt == shard.index.end()) {
        ++misses_;
        return false;
    }

    if (expired(*indexIt->second.iterator, Clock::now())) {
        remove_locked(shard, indexIt, false, true);
        ++misses_;
        return false;
    }

    // 先复制 value，再移动链表节点。list::splice 不会使节点迭代器失效，但先复制
    // 可让“读取结果”和“更新策略状态”的步骤更加清晰。
    value = indexIt->second.iterator->value;
    touch_locked(shard, indexIt);
    ++hits_;
    return true;
}

/**
 * @brief 写入新值或更新已有值。
 *
 * 已有 key 更新原节点并按一次访问处理；新 key 进入 Window 头部。Window 超限时，
 * drain_window_locked() 会取尾部 candidate 执行 TinyLFU 准入比较。
 */
void ShardedWTinyLfuCache::put(const std::string& key,
                               const std::string& value,
                               int ttlSeconds)
{
    // make_expire_time 通过引用参数同时返回“绝对过期时间”和“是否永不过期”。
    bool neverExpire = false;
    const TimePoint expireAt = make_expire_time(ttlSeconds, neverExpire);

    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    // 写入前清理本分片过期项，尽量先回收无效空间，再决定是否需要容量淘汰。
    prune_expired_locked(shard, Clock::now());

    auto indexIt = shard.index.find(key);
    if (indexIt != shard.index.end()) {
        // 解引用 list iterator 得到原 Entry 的引用，后续赋值直接修改缓存节点。
        Entry& entry = *indexIt->second.iterator;
        entry.value = value;
        entry.expireAt = expireAt;
        entry.neverExpire = neverExpire;
        touch_locked(shard, indexIt);
        return;
    }

    // 访问频率统一由 get() 记录。正常缓存链路和 Redis 回填都先查询 L1 再写入，
    // 因此这里不重复计数，避免一次未命中请求被误算为两次访问。
    // 聚合初始化 Entry 后插入 Window 头部；push_front 完成后 begin() 就指向新节点。
    shard.window.push_front(Entry{key, value, expireAt, neverExpire});
    // emplace 直接在 unordered_map 内构造 pair，Location 同时记录分段和节点迭代器。
    shard.index.emplace(key, Location{Segment::Window, shard.window.begin()});
    drain_window_locked(shard);
}

/**
 * @brief 删除指定缓存值。
 */
void ShardedWTinyLfuCache::erase(const std::string& key)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto indexIt = shard.index.find(key);
    if (indexIt != shard.index.end()) {
        remove_locked(shard, indexIt, false, false);
    }
    // Count-Min Sketch 是概率型共享计数结构，无法也不应该为单个 key 清零。
    // erase 只删除缓存值；残留频率会在后续 Frequency Aging 中逐步衰减。
}

/**
 * @brief 原子读取累计统计并返回按值快照。
 *
 * 各字段可能由不同工作线程继续更新，因此该快照不保证所有字段来自完全相同的
 * CPU 时刻，但每个字段本身都不会发生数据竞争，足以用于日志和测试。
 */
ShardedWTinyLfuCache::Stats ShardedWTinyLfuCache::stats() const
{
    return Stats{hits_.load(),
                 misses_.load(),
                 admissions_.load(),
                 rejections_.load(),
                 evictions_.load(),
                 expirations_.load(),
                 frequencyAges_.load()};
}

/**
 * @brief 根据 key 的稳定哈希值选择所属分片。
 */
ShardedWTinyLfuCache::Shard& ShardedWTinyLfuCache::shard_for(const std::string& key)
{
    // 构造函数保证 shards_ 非空，因此取模不会除以 0。vector 保存 unique_ptr，
    // 第一次 * 解引用 unique_ptr，最终返回 Shard&。
    return *shards_[hash_(key) % shards_.size()];
}

/**
 * @brief 判断节点在给定单调时钟时间点是否过期。
 */
bool ShardedWTinyLfuCache::expired(const Entry& entry, TimePoint now)
{
    return !entry.neverExpire && now >= entry.expireAt;
}

/**
 * @brief 将 TTL 秒数转换为 steady_clock 绝对时间点。
 */
ShardedWTinyLfuCache::TimePoint
ShardedWTinyLfuCache::make_expire_time(int ttlSeconds, bool& neverExpire)
{
    if (ttlSeconds <= 0) {
        // TimePoint::max() 仅作占位。expired() 会先检查 neverExpire，不会真的等待到
        // 最大时间点才判断。
        neverExpire = true;
        return TimePoint::max();
    }
    neverExpire = false;
    return Clock::now() + std::chrono::seconds(ttlSeconds);
}

/**
 * @brief 返回 segment 对应的实际链表。
 *
 * 返回引用使调用方可以直接 splice/erase 原链表；不会复制整条缓存链表。
 */
ShardedWTinyLfuCache::EntryList&
ShardedWTinyLfuCache::list_for(Shard& shard, Segment segment)
{
    if (segment == Segment::Window) {
        return shard.window;
    }
    if (segment == Segment::Probation) {
        return shard.probation;
    }
    return shard.protectedEntries;
}

/**
 * @brief 在已持有分片锁时记录一次访问频率。
 */
void ShardedWTinyLfuCache::record_frequency_locked(Shard& shard, const std::string& key)
{
    const std::uint64_t hash = static_cast<std::uint64_t>(hash_(key));
    if (shard.frequencySketch.increment(hash)) {
        ++frequencyAges_;
    }
}

/**
 * @brief 在已持有分片锁时读取 key 的近似频率。
 */
std::uint32_t ShardedWTinyLfuCache::frequency_locked(const Shard& shard,
                                                     const std::string& key) const
{
    // 与 shard_for()/record_frequency_locked() 复用同一个 hash_ 对象，确保分片、
    // 记录和估算三个环节对 key 的哈希语义完全一致。
    return shard.frequencySketch.estimate(static_cast<std::uint64_t>(hash_(key)));
}

/**
 * @brief 处理命中后的分段访问顺序变化。
 *
 * Window/Protected 命中只移动到本段头部；Probation 命中会晋升 Protected；若
 * Protected 超过上限，再把其尾部节点降级到 Probation 头部。
 */
void ShardedWTinyLfuCache::touch_locked(
    Shard& shard,
    std::unordered_map<std::string, Location>::iterator indexIt)
{
    // Location 使用引用，后续 segment/iterator 更新会直接写回 index。
    Location& location = indexIt->second;
    if (location.segment == Segment::Window) {
        // list::splice(pos, source, iterator) 在 O(1) 时间移动已有节点，不复制 Entry。
        shard.window.splice(shard.window.begin(), shard.window, location.iterator);
        location.iterator = shard.window.begin();
        return;
    }

    if (location.segment == Segment::Protected) {
        shard.protectedEntries.splice(shard.protectedEntries.begin(),
                                      shard.protectedEntries,
                                      location.iterator);
        location.iterator = shard.protectedEntries.begin();
        return;
    }

    if (shard.protectedCapacity == 0) {
        // 极小分片没有 Protected 空间，此时 Main 退化为 Probation LRU。
        shard.probation.splice(shard.probation.begin(), shard.probation, location.iterator);
        location.iterator = shard.probation.begin();
        return;
    }

    // Probation 再次命中后晋升 Protected，表明它不是一次性扫描数据。
    shard.protectedEntries.splice(shard.protectedEntries.begin(),
                                  shard.probation,
                                  location.iterator);
    location.segment = Segment::Protected;
    location.iterator = shard.protectedEntries.begin();

    if (shard.protectedEntries.size() <= shard.protectedCapacity) {
        return;
    }

    // Protected 超限时把其 LRU 尾部降级到 Probation 头部，而不是直接删除。
    // end() 指向尾后位置，std::prev(end()) 才是最后一个有效节点，即当前 LRU。
    auto demoted = std::prev(shard.protectedEntries.end());
    const std::string demotedKey = demoted->key;
    shard.probation.splice(shard.probation.begin(), shard.protectedEntries, demoted);
    auto demotedIndex = shard.index.find(demotedKey);
    demotedIndex->second.segment = Segment::Probation;
    demotedIndex->second.iterator = shard.probation.begin();
}

/**
 * @brief 同时删除链表节点和哈希索引，并更新删除原因统计。
 */
void ShardedWTinyLfuCache::remove_locked(
    Shard& shard,
    std::unordered_map<std::string, Location>::iterator indexIt,
    bool eviction,
    bool expiration)
{
    // 必须先从 list 删除节点，再从 index 删除保存该 list iterator 的映射。
    EntryList& entries = list_for(shard, indexIt->second.segment);
    entries.erase(indexIt->second.iterator);
    shard.index.erase(indexIt);
    if (eviction) {
        ++evictions_;
    }
    if (expiration) {
        ++expirations_;
    }
}

/**
 * @brief 惰性遍历并清理分片三个链表中的所有过期节点。
 */
void ShardedWTinyLfuCache::prune_expired_locked(Shard& shard, TimePoint now)
{
    // 固定数组让三段使用同一套删除逻辑，避免分别复制三段循环。
    const Segment segments[] = {Segment::Window, Segment::Probation, Segment::Protected};
    for (Segment segment : segments) {
        EntryList& entries = list_for(shard, segment);
        for (auto it = entries.begin(); it != entries.end();) {
            if (!expired(*it, now)) {
                ++it;
                continue;
            }
            // list::erase 会使当前节点迭代器失效，所以先用 key 删除 index，再利用
            // erase 的返回值取得下一个有效迭代器。
            shard.index.erase(it->key);
            it = entries.erase(it);
            ++expirations_;
        }
    }
}

/**
 * @brief 将 Window 大小压回配置上限，并把尾部节点作为 Main 候选。
 */
void ShardedWTinyLfuCache::drain_window_locked(Shard& shard)
{
    while (shard.window.size() > shard.windowCapacity) {
        auto candidateIt = std::prev(shard.window.end());
        // Entry 含多个 string。std::move 转移字符串缓冲区，避免 candidate 产生深拷贝。
        Entry candidate = std::move(*candidateIt);
        // candidate 已从 Window 所属结构移出，必须先同步删除旧 index，再尝试进入 Main。
        shard.index.erase(candidate.key);
        shard.window.erase(candidateIt);
        admit_to_main_locked(shard, std::move(candidate));
    }
}

/**
 * @brief 根据 Main 容量与 TinyLFU 频率完成 candidate 准入。
 */
void ShardedWTinyLfuCache::admit_to_main_locked(Shard& shard, Entry candidate)
{
    if (shard.mainCapacity == 0) {
        // 分片容量为 1 时全部空间属于 Window，不存在 Main。被挤出的 candidate
        // 只能丢弃，同时计为一次拒绝和一次容量淘汰。
        ++rejections_;
        ++evictions_;
        return;
    }

    const std::size_t mainSize = shard.probation.size() + shard.protectedEntries.size();
    if (mainSize < shard.mainCapacity) {
        // Main 尚有空位时无需牺牲 victim，candidate 直接成为 Probation MRU。
        shard.probation.push_front(std::move(candidate));
        shard.index.emplace(shard.probation.front().key,
                            Location{Segment::Probation, shard.probation.begin()});
        ++admissions_;
        return;
    }

    // Main 已满时，Probation 尾部是最弱的 Main 候选。只有 candidate 的估算
    // 频率严格更高才允许替换；相同频率时保留已有数据，优先抵抗一次性扫描。
    if (shard.probation.empty()) {
        ++rejections_;
        ++evictions_;
        return;
    }

    auto victimIt = std::prev(shard.probation.end());
    if (frequency_locked(shard, candidate.key) <=
        frequency_locked(shard, victimIt->key)) {
        ++rejections_;
        ++evictions_;
        return;
    }

    // candidate 胜出后先从 index 删除 victim。必须在 list::erase 之前读取 key，
    // 因为链表节点销毁后 victimIt 会失效，不能再通过 victimIt->key 访问节点。
    shard.index.erase(victimIt->key);
    shard.probation.erase(victimIt);
    ++evictions_;

    shard.probation.push_front(std::move(candidate));
    shard.index.emplace(shard.probation.front().key,
                        Location{Segment::Probation, shard.probation.begin()});
    ++admissions_;
}
