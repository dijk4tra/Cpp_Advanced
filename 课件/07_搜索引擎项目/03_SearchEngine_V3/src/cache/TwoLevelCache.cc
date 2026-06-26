#include "../../include/cache/TwoLevelCache.h"

/**
 * @brief 构造二级缓存。
 *
 * l1 和 l2 都是非拥有型指针，TwoLevelCache 不负责释放它们。online_main 中用
 * unique_ptr 持有真实缓存对象，并保证生命周期长于 TwoLevelCache。
 */
TwoLevelCache::TwoLevelCache(Cache* l1, Cache* l2, int l1BackfillTtlSeconds)
    : l1_(l1)
    , l2_(l2)
    , l1BackfillTtlSeconds_(l1BackfillTtlSeconds)
{
}

/**
 * @brief 查询二级缓存。
 */
bool TwoLevelCache::get(const std::string& key, std::string& value)
{
    // 优先查询 L1。本地缓存命中最快，不需要访问 Redis。
    if (l1_ != nullptr && l1_->get(key, value)) {
        return true;
    }

    // L1 未命中时查询 L2。Redis 命中后回填 L1，后续相同 key 可以直接在本地命中。
    if (l2_ != nullptr && l2_->get(key, value)) {
        if (l1_ != nullptr) {
            // 回填 TTL 使用配置的 redis_l1_backfill_ttl_seconds。它不一定等于 Redis
            // 中剩余 TTL，但能保证热数据重新进入本地缓存。
            l1_->put(key, value, l1BackfillTtlSeconds_);
        }
        return true;
    }

    return false;
}

/**
 * @brief 同时写入两级缓存。
 */
void TwoLevelCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    // 先写 L1，保证当前进程后续查询尽快命中。
    if (l1_ != nullptr) {
        l1_->put(key, value, ttlSeconds);
    }
    // 再写 L2，实现跨进程或重启后的共享缓存。Redis 写失败由 RedisCache 内部吞掉。
    if (l2_ != nullptr) {
        l2_->put(key, value, ttlSeconds);
    }
}

/**
 * @brief 同时删除两级缓存。
 */
void TwoLevelCache::erase(const std::string& key)
{
    if (l1_ != nullptr) {
        l1_->erase(key);
    }
    if (l2_ != nullptr) {
        l2_->erase(key);
    }
}
