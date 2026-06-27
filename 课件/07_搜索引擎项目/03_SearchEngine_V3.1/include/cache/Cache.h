#pragma once

#include <string>

/**
 * @brief 缓存层统一接口。
 *
 * 第三期先实现本地 L1 缓存，后续 Redis L2 缓存也复用该接口。缓存只保存可重建
 * 的派生数据，例如关键词推荐 JSON、网页搜索 JSON。缓存未命中时调用方应回退到
 * 原始搜索流程。
 */
class Cache
{
public:
    virtual ~Cache() = default;

    /**
     * @brief 根据 key 查询缓存。
     * @param key 缓存 key。
     * @param value 命中时写入缓存值。
     * @return 命中且未过期返回 true，否则返回 false。
     */
    virtual bool get(const std::string& key, std::string& value) = 0;

    /**
     * @brief 写入缓存。
     * @param key 缓存 key。
     * @param value 缓存值。
     * @param ttlSeconds 存活秒数；小于等于 0 表示不过期。
     */
    virtual void put(const std::string& key, const std::string& value, int ttlSeconds) = 0;

    /**
     * @brief 删除指定缓存项。
     */
    virtual void erase(const std::string& key) = 0;
};
