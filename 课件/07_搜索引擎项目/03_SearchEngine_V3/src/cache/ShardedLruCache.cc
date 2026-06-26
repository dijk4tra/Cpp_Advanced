#include "../../include/cache/ShardedLruCache.h"

#include <algorithm>

ShardedLruCache::ShardedLruCache(std::size_t capacity, std::size_t shardCount)
    : capacity_(std::max<std::size_t>(1, capacity))
{
    shardCount = std::max<std::size_t>(1, shardCount);
    shardCount = std::min(shardCount, capacity_);
    std::size_t baseCapacity = capacity_ / shardCount;
    std::size_t extra = capacity_ % shardCount;

    shards_.reserve(shardCount);
    for (std::size_t i = 0; i < shardCount; ++i) {
        std::size_t shardCapacity = baseCapacity + (i < extra ? 1 : 0);
        shardCapacity = std::max<std::size_t>(1, shardCapacity);
        shards_.push_back(std::make_unique<Shard>(shardCapacity));
    }
}

ShardedLruCache::~ShardedLruCache() = default;

bool ShardedLruCache::get(const std::string& key, std::string& value)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.index.find(key);
    if (it == shard.index.end()) {
        return false;
    }

    auto now = Clock::now();
    auto listIt = it->second;
    if (expired(*listIt, now)) {
        shard.entries.erase(listIt);
        shard.index.erase(it);
        return false;
    }

    shard.entries.splice(shard.entries.begin(), shard.entries, listIt);
    value = shard.entries.begin()->value;
    return true;
}

void ShardedLruCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    bool neverExpire = false;
    TimePoint expireAt = make_expire_time(ttlSeconds, neverExpire);

    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto now = Clock::now();
    auto it = shard.index.find(key);
    if (it != shard.index.end()) {
        auto listIt = it->second;
        listIt->value = value;
        listIt->expireAt = expireAt;
        listIt->neverExpire = neverExpire;
        shard.entries.splice(shard.entries.begin(), shard.entries, listIt);
        return;
    }

    prune_expired_locked(shard, now);
    shard.entries.push_front(Entry{key, value, expireAt, neverExpire});
    shard.index[key] = shard.entries.begin();
    evict_to_capacity_locked(shard);
}

void ShardedLruCache::erase(const std::string& key)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.index.find(key);
    if (it == shard.index.end()) {
        return;
    }

    shard.entries.erase(it->second);
    shard.index.erase(it);
}

ShardedLruCache::Shard& ShardedLruCache::shard_for(const std::string& key)
{
    std::size_t index = hash_(key) % shards_.size();
    return *shards_[index];
}

bool ShardedLruCache::expired(const Entry& entry, TimePoint now)
{
    return !entry.neverExpire && now >= entry.expireAt;
}

ShardedLruCache::TimePoint ShardedLruCache::make_expire_time(int ttlSeconds, bool& neverExpire)
{
    if (ttlSeconds <= 0) {
        neverExpire = true;
        return TimePoint::max();
    }

    neverExpire = false;
    return Clock::now() + std::chrono::seconds(ttlSeconds);
}

void ShardedLruCache::prune_expired_locked(Shard& shard, TimePoint now)
{
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (!expired(*it, now)) {
            ++it;
            continue;
        }

        shard.index.erase(it->key);
        it = shard.entries.erase(it);
    }
}

void ShardedLruCache::evict_to_capacity_locked(Shard& shard)
{
    while (shard.entries.size() > shard.capacity) {
        auto last = std::prev(shard.entries.end());
        shard.index.erase(last->key);
        shard.entries.pop_back();
    }
}
