#pragma once

#include "Cache.h"

/**
 * @brief 二级缓存组合。
 *
 * get 时先查 L1，本地未命中再查 L2；L2 命中后回填 L1。put/erase 同时写入两级。
 * 两级缓存对象由调用方持有，生命周期必须长于 TwoLevelCache。
 */
class TwoLevelCache : public Cache
{
public:
    /**
     * @param l1 一级本地缓存，可以为 nullptr。
     * @param l2 二级 Redis 缓存，可以为 nullptr。
     * @param l1BackfillTtlSeconds L2 命中后回填 L1 使用的 TTL。
     */
    TwoLevelCache(Cache* l1, Cache* l2, int l1BackfillTtlSeconds);

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

private:
    // L1 当前通常是进程内 ShardedWTinyLfuCache，也可配置回退到 ShardedLruCache。
    Cache* l1_;

    // L2 通常是 RedisCache，用于跨进程共享或服务重启后保留一部分热数据。
    Cache* l2_;

    // Redis 命中回填本地缓存的 TTL。单独配置可避免 L1 保存时间过长。
    int l1BackfillTtlSeconds_;
};
