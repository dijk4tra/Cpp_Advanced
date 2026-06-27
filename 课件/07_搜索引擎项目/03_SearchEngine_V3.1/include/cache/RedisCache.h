#pragma once

#include "Cache.h"

#include <string>

/**
 * @brief 基于 hiredis 的 Redis 缓存客户端。
 *
 * 当前实现每次命令使用短连接，避免多线程共享连接的复杂状态。Redis 异常时
 * get 返回 false，put/erase 静默失败，使 Redis 始终只是加速层，不影响搜索主流程。
 */
class RedisCache : public Cache
{
public:
    /**
     * @param host Redis 主机地址。
     * @param port Redis 端口。
     * @param db Redis 数据库编号。
     * @param connectTimeoutMs 建立 TCP 连接的超时时间，单位毫秒。
     * @param commandTimeoutMs 单条 Redis 命令的读写超时时间，单位毫秒。
     */
    RedisCache(std::string host,
               int port,
               int db,
               int connectTimeoutMs,
               int commandTimeoutMs);

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

private:
    /**
     * @brief 创建一条 hiredis 短连接。
     *
     * 返回裸指针是 hiredis C API 的约定，调用方必须通过 redisFree 释放。
     */
    struct redisContext* connect() const;

    /**
     * @brief 按配置切换 Redis DB。
     *
     * db_ 为 0 时不需要发送 SELECT 命令。
     */
    bool select_db(struct redisContext* context) const;

private:
    // Redis 连接参数。对象构造后只读，因此多线程访问这些成员不需要加锁。
    std::string host_;
    int port_;
    int db_;

    // 超时设置越小，Redis 异常时对搜索主流程的拖累越小。
    int connectTimeoutMs_;
    int commandTimeoutMs_;
};
