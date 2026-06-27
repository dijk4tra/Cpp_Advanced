#include "../../include/cache/ShardedLruCache.h"

#include <algorithm>

/**
 * @brief 构造分片 LRU 缓存。
 *
 * capacity 是整个缓存允许保存的最大 key 数；shardCount 是分片数量。分片数越多，
 * 不同 key 落到不同锁上的概率越高，多线程争抢同一把锁的概率越低。但分片过多
 * 会让每个分片容量过小，因此这里把 shardCount 限制为不超过 capacity_。
 */
ShardedLruCache::ShardedLruCache(std::size_t capacity, std::size_t shardCount)
    // std::max<std::size_t>(1, capacity) 保证容量至少为 1，避免后面除以 0 或
    // 创建没有任何可用空间的缓存。模板实参 <std::size_t> 用于明确比较类型。
    : capacity_(std::max<std::size_t>(1, capacity))
{
    // 分片数至少为 1；同时不超过总容量，避免“容量配置 2、分片 32”时每个分片
    // 都被强制分到 1 个容量，最终实际容量变成 32。
    shardCount = std::max<std::size_t>(1, shardCount);
    shardCount = std::min(shardCount, capacity_);

    // baseCapacity 是每个分片的基础容量；extra 是不能整除时剩下的容量。
    // 前 extra 个分片每个多分 1 个位置，使所有分片容量总和严格等于 capacity_。
    std::size_t baseCapacity = capacity_ / shardCount;
    std::size_t extra = capacity_ % shardCount;

    // reserve 只预留 vector 容量，不会改变 size。这里提前预留可避免 push_back
    // 时多次扩容；unique_ptr 保存每个 Shard，避免 Shard 内部 mutex 被移动。
    shards_.reserve(shardCount);
    for (std::size_t i = 0; i < shardCount; ++i) {
        std::size_t shardCapacity = baseCapacity + (i < extra ? 1 : 0);
        shardCapacity = std::max<std::size_t>(1, shardCapacity);
        shards_.push_back(std::make_unique<Shard>(shardCapacity));
    }
}

ShardedLruCache::~ShardedLruCache() = default;

/**
 * @brief 查询缓存。
 *
 * LRU 的 get 不是纯读操作：命中后要把节点移动到链表头部，表示最近访问。因此
 * 即使只是查询，也必须持有对应分片的互斥锁。
 */
bool ShardedLruCache::get(const std::string& key, std::string& value)
{
    // 先根据 key 的哈希值定位分片。只有这个分片会被加锁，其他分片仍可并发访问。
    Shard& shard = shard_for(key);
    // lock_guard 使用 RAII 管理 mutex：构造时加锁，离开作用域自动解锁。
    std::lock_guard<std::mutex> lock(shard.mutex);

    // index 保存 key -> list iterator。unordered_map 平均 O(1) 查找，链表迭代器
    // 可直接定位到 LRU 链表中的节点。
    auto it = shard.index.find(key);
    if (it == shard.index.end()) {
        return false;
    }

    auto now = Clock::now();
    auto listIt = it->second;
    if (expired(*listIt, now)) {
        // 发现过期时立即惰性删除。必须同时删除链表节点和哈希索引，否则会留下
        // 指向已删除节点的失效迭代器。
        shard.entries.erase(listIt);
        shard.index.erase(it);
        return false;
    }

    // list::splice 可以在 O(1) 时间内把已有节点移动到链表头部，不复制 Entry，
    // 也不重新分配字符串内存。头部表示最近使用，尾部表示最久未使用。
    shard.entries.splice(shard.entries.begin(), shard.entries, listIt);
    // value 是输出参数，命中后复制缓存字符串给调用者。缓存内部仍保留自己的副本。
    value = shard.entries.begin()->value;
    return true;
}

/**
 * @brief 写入或更新缓存。
 */
void ShardedLruCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    bool neverExpire = false;
    // 根据 ttlSeconds 计算绝对过期时间。neverExpire 通过引用参数写回，用于区分
    // “不过期”和“有具体过期时间”两类节点。
    TimePoint expireAt = make_expire_time(ttlSeconds, neverExpire);

    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto now = Clock::now();
    auto it = shard.index.find(key);
    if (it != shard.index.end()) {
        // key 已存在时只更新 value 和过期时间，再移动到链表头部。这样一次 put
        // 同时表示“写入”和“最近访问”。
        auto listIt = it->second;
        listIt->value = value;
        listIt->expireAt = expireAt;
        listIt->neverExpire = neverExpire;
        shard.entries.splice(shard.entries.begin(), shard.entries, listIt);
        return;
    }

    // 新插入前先清理本分片中已经过期的节点，可以减少后续容量淘汰时误删仍有效
    // 的旧热点数据。这里没有全局定时清理线程，采用写入时惰性清理。
    prune_expired_locked(shard, now);
    // 新节点插入链表头部，表示刚刚被访问。
    shard.entries.push_front(Entry{key, value, expireAt, neverExpire});
    // 保存 key 到链表节点迭代器的映射，之后 get/erase 都能 O(1) 定位。
    shard.index[key] = shard.entries.begin();
    // 如果插入后超过分片容量，从链表尾部淘汰最久未使用节点。
    evict_to_capacity_locked(shard);
}

/**
 * @brief 删除指定 key。
 */
void ShardedLruCache::erase(const std::string& key)
{
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.index.find(key);
    if (it == shard.index.end()) {
        return;
    }

    // 与过期删除一样，必须同时维护 list 和 unordered_map 的一致性。
    shard.entries.erase(it->second);
    shard.index.erase(it);
}

/**
 * @brief 根据 key 选择分片。
 */
ShardedLruCache::Shard& ShardedLruCache::shard_for(const std::string& key)
{
    // std::hash<std::string> 将字符串映射为 size_t，再对分片数取模。相同 key
    // 一定落到同一分片，不同 key 尽量均匀分布到多个分片。
    std::size_t index = hash_(key) % shards_.size();
    return *shards_[index];
}

/**
 * @brief 判断节点是否已经过期。
 */
bool ShardedLruCache::expired(const Entry& entry, TimePoint now)
{
    // neverExpire 为 true 时忽略 expireAt；否则用 steady_clock 的时间点比较。
    // steady_clock 单调递增，不受系统时间被手动调整影响，适合做 TTL。
    return !entry.neverExpire && now >= entry.expireAt;
}

/**
 * @brief 根据 TTL 计算过期时间。
 */
ShardedLruCache::TimePoint ShardedLruCache::make_expire_time(int ttlSeconds, bool& neverExpire)
{
    if (ttlSeconds <= 0) {
        // 约定 ttlSeconds <= 0 表示不过期。TimePoint::max() 只是占位，真正判断
        // 是否过期时优先看 neverExpire。
        neverExpire = true;
        return TimePoint::max();
    }

    neverExpire = false;
    // Clock::now() + seconds 得到绝对过期时间。后续 get/put 清理时只比较时间点。
    return Clock::now() + std::chrono::seconds(ttlSeconds);
}

/**
 * @brief 清理某个分片中的过期节点。
 *
 * 函数名里的 locked 表示调用者必须已经持有 shard.mutex。本函数不再重复加锁，
 * 避免同一线程重复锁同一把非递归 mutex。
 */
void ShardedLruCache::prune_expired_locked(Shard& shard, TimePoint now)
{
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (!expired(*it, now)) {
            ++it;
            continue;
        }

        // erase 返回被删除节点后面的下一个迭代器，适合在遍历链表时安全删除。
        shard.index.erase(it->key);
        it = shard.entries.erase(it);
    }
}

/**
 * @brief 将分片大小压回容量限制以内。
 */
void ShardedLruCache::evict_to_capacity_locked(Shard& shard)
{
    while (shard.entries.size() > shard.capacity) {
        // 链表尾部是最久未访问节点。std::prev(end) 取得最后一个有效节点迭代器。
        auto last = std::prev(shard.entries.end());
        shard.index.erase(last->key);
        shard.entries.pop_back();
    }
}
