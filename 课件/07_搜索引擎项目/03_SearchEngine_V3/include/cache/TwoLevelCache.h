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
    TwoLevelCache(Cache* l1, Cache* l2, int l1BackfillTtlSeconds);

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

private:
    Cache* l1_;
    Cache* l2_;
    int l1BackfillTtlSeconds_;
};
