#pragma once

#include "Cache.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief 基于 hiredis 的 Redis 缓存客户端。
 *
 * RedisCache 使用有上限的持久连接池。每条 hiredis 同步连接同一时刻只会借给
 * 一个工作线程，命令完成后归还；连接错误时丢弃并在后续请求中惰性重建。
 * Redis 异常时 get 返回 false，put/erase 静默失败，不影响搜索主流程。
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
     * @param poolSize 最大持久连接数。
     * @param poolWaitTimeoutMs 连接池耗尽时等待空闲连接的最长时间。
     */
    RedisCache(std::string host,
               int port,
               int db,
               int connectTimeoutMs,
               int commandTimeoutMs,
               int poolSize,
               int poolWaitTimeoutMs);

    ~RedisCache() override;

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

private:
    /**
     * @brief 创建、设置超时并选择 DB 的持久连接。
     *
     * 返回裸指针是 hiredis C API 的约定，调用方必须通过 redisFree 释放。
     */
    struct redisContext* connect() const;

    /** @brief 从连接池借出一条独占连接，超时或建连失败时返回 nullptr。 */
    struct redisContext* acquire();

    /** @brief 归还健康连接；异常连接会被关闭并从池容量中移除。 */
    void release(struct redisContext* context, bool healthy);

    /** @brief 首次以及每累计 100 次故障记录一条 WARN，避免 Redis 故障刷屏。 */
    void log_failure(const char* reason);

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

    // 连接池容量和等待策略。
    std::size_t poolSize_;
    int poolWaitTimeoutMs_;

    // idleConnections_ 中的连接当前没有线程使用。totalConnections_ 同时统计空闲
    // 和已借出连接，用于保证惰性建连时不会突破 poolSize_。
    std::mutex poolMutex_;
    std::condition_variable poolCondition_;
    std::vector<struct redisContext*> idleConnections_;
    std::size_t totalConnections_ = 0;
    std::atomic<std::uint64_t> failureCount_{0};
};
