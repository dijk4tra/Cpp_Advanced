#include "../../include/cache/ShardedWTinyLfuCache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
std::size_t clamp_percent(std::size_t value)
{
    return std::max<std::size_t>(1, std::min<std::size_t>(99, value));
}
}

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
    shardCount = std::max<std::size_t>(1, shardCount);
    shardCount = std::min(shardCount, capacity_);

    const std::size_t baseCapacity = capacity_ / shardCount;
    const std::size_t extra = capacity_ % shardCount;
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
        shards_.push_back(std::make_unique<Shard>(shardCapacity,
                                                  windowCapacity,
                                                  protectedCapacity,
                                                  sampleSize));
    }
}

ShardedWTinyLfuCache::~ShardedWTinyLfuCache() = default;

bool ShardedWTinyLfuCache::get(const std::string& key, std::string& value)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    // TinyLFU 必须同时观察命中和未命中访问。只统计已经缓存的 key 会使新热点
    // 永远没有足够频率通过准入比较。
    record_frequency_locked(shard, key);

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

    value = indexIt->second.iterator->value;
    touch_locked(shard, indexIt);
    ++hits_;
    return true;
}

void ShardedWTinyLfuCache::put(const std::string& key,
                               const std::string& value,
                               int ttlSeconds)
{
    bool neverExpire = false;
    const TimePoint expireAt = make_expire_time(ttlSeconds, neverExpire);

    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    prune_expired_locked(shard, Clock::now());

    auto indexIt = shard.index.find(key);
    if (indexIt != shard.index.end()) {
        Entry& entry = *indexIt->second.iterator;
        entry.value = value;
        entry.expireAt = expireAt;
        entry.neverExpire = neverExpire;
        touch_locked(shard, indexIt);
        return;
    }

    // 正常调用路径通常先 get 未命中再 put，此时频率已经记录。该判断兼容直接
    // put 的调用者，保证新 key 至少拥有一次访问频率，又避免一次请求重复计数。
    if (shard.frequencies.find(key) == shard.frequencies.end()) {
        record_frequency_locked(shard, key);
    }

    shard.window.push_front(Entry{key, value, expireAt, neverExpire});
    shard.index.emplace(key, Location{Segment::Window, shard.window.begin()});
    drain_window_locked(shard);
}

void ShardedWTinyLfuCache::erase(const std::string& key)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto indexIt = shard.index.find(key);
    if (indexIt != shard.index.end()) {
        remove_locked(shard, indexIt, false, false);
    }
    shard.frequencies.erase(key);
}

ShardedWTinyLfuCache::Stats ShardedWTinyLfuCache::stats() const
{
    return Stats{hits_.load(),
                 misses_.load(),
                 admissions_.load(),
                 rejections_.load(),
                 evictions_.load(),
                 expirations_.load()};
}

ShardedWTinyLfuCache::Shard& ShardedWTinyLfuCache::shard_for(const std::string& key)
{
    return *shards_[hash_(key) % shards_.size()];
}

bool ShardedWTinyLfuCache::expired(const Entry& entry, TimePoint now)
{
    return !entry.neverExpire && now >= entry.expireAt;
}

ShardedWTinyLfuCache::TimePoint
ShardedWTinyLfuCache::make_expire_time(int ttlSeconds, bool& neverExpire)
{
    if (ttlSeconds <= 0) {
        neverExpire = true;
        return TimePoint::max();
    }
    neverExpire = false;
    return Clock::now() + std::chrono::seconds(ttlSeconds);
}

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

void ShardedWTinyLfuCache::record_frequency_locked(Shard& shard, const std::string& key)
{
    std::uint32_t& count = shard.frequencies[key];
    if (count != std::numeric_limits<std::uint32_t>::max()) {
        ++count;
    }
    ++shard.frequencySamples;

    if (shard.frequencySamples < shard.frequencySampleSize) {
        return;
    }

    // 周期减半让旧热点逐渐失去历史优势，同时删除衰减为 0 的冷 key，避免当前
    // 阶段的精确频率表随着长尾查询无限增长。
    for (auto it = shard.frequencies.begin(); it != shard.frequencies.end();) {
        it->second /= 2;
        if (it->second == 0) {
            it = shard.frequencies.erase(it);
        } else {
            ++it;
        }
    }
    shard.frequencySamples = 0;
}

std::uint32_t ShardedWTinyLfuCache::frequency_locked(const Shard& shard,
                                                     const std::string& key)
{
    auto it = shard.frequencies.find(key);
    return it == shard.frequencies.end() ? 0 : it->second;
}

void ShardedWTinyLfuCache::touch_locked(
    Shard& shard,
    std::unordered_map<std::string, Location>::iterator indexIt)
{
    Location& location = indexIt->second;
    if (location.segment == Segment::Window) {
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
    auto demoted = std::prev(shard.protectedEntries.end());
    const std::string demotedKey = demoted->key;
    shard.probation.splice(shard.probation.begin(), shard.protectedEntries, demoted);
    auto demotedIndex = shard.index.find(demotedKey);
    demotedIndex->second.segment = Segment::Probation;
    demotedIndex->second.iterator = shard.probation.begin();
}

void ShardedWTinyLfuCache::remove_locked(
    Shard& shard,
    std::unordered_map<std::string, Location>::iterator indexIt,
    bool eviction,
    bool expiration)
{
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

void ShardedWTinyLfuCache::prune_expired_locked(Shard& shard, TimePoint now)
{
    const Segment segments[] = {Segment::Window, Segment::Probation, Segment::Protected};
    for (Segment segment : segments) {
        EntryList& entries = list_for(shard, segment);
        for (auto it = entries.begin(); it != entries.end();) {
            if (!expired(*it, now)) {
                ++it;
                continue;
            }
            shard.index.erase(it->key);
            it = entries.erase(it);
            ++expirations_;
        }
    }
}

void ShardedWTinyLfuCache::drain_window_locked(Shard& shard)
{
    while (shard.window.size() > shard.windowCapacity) {
        auto candidateIt = std::prev(shard.window.end());
        Entry candidate = std::move(*candidateIt);
        shard.index.erase(candidate.key);
        shard.window.erase(candidateIt);
        admit_to_main_locked(shard, std::move(candidate));
    }
}

void ShardedWTinyLfuCache::admit_to_main_locked(Shard& shard, Entry candidate)
{
    if (shard.mainCapacity == 0) {
        ++rejections_;
        ++evictions_;
        return;
    }

    const std::size_t mainSize = shard.probation.size() + shard.protectedEntries.size();
    if (mainSize < shard.mainCapacity) {
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

    shard.index.erase(victimIt->key);
    shard.probation.erase(victimIt);
    ++evictions_;

    shard.probation.push_front(std::move(candidate));
    shard.index.emplace(shard.probation.front().key,
                        Location{Segment::Probation, shard.probation.begin()});
    ++admissions_;
}
