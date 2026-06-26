#pragma once

#include "Cache.h"

#include <string>
#include <vector>

/**
 * @brief 最小 Redis 缓存客户端。
 *
 * 当前实现只支持本项目需要的 GET、SET/SETEX、DEL。每次命令使用短连接，避免
 * 引入第三方 Redis 客户端依赖，也避免多线程共享连接的复杂状态。Redis 异常时
 * get 返回 false，put/erase 静默失败，使 Redis 始终只是加速层。
 */
class RedisCache : public Cache
{
public:
    RedisCache(std::string host,
               int port,
               int db,
               int connectTimeoutMs,
               int commandTimeoutMs);

    bool get(const std::string& key, std::string& value) override;
    void put(const std::string& key, const std::string& value, int ttlSeconds) override;
    void erase(const std::string& key) override;

private:
    struct Reply {
        enum class Type {
            SimpleString,
            Error,
            Integer,
            BulkString,
            Nil,
            Invalid
        };

        Type type = Type::Invalid;
        std::string text;
        long long integer = 0;
    };

private:
    bool execute(const std::vector<std::string>& command, Reply& reply) const;
    bool select_db(int fd) const;
    int connect_socket() const;
    bool send_command(int fd, const std::vector<std::string>& command) const;
    bool send_all(int fd, const std::string& data) const;
    bool read_reply(int fd, Reply& reply) const;
    bool read_line(int fd, std::string& line) const;
    bool read_exact(int fd, std::size_t size, std::string& output) const;

private:
    std::string host_;
    int port_;
    int db_;
    int connectTimeoutMs_;
    int commandTimeoutMs_;
};
