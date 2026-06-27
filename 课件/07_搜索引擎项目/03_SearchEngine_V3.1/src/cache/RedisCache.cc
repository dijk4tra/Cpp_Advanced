#include "../../include/cache/RedisCache.h"

#include <hiredis/hiredis.h>

#include <memory>
#include <sys/time.h>
#include <utility>

namespace
{
// hiredis 的 redisContext 需要使用 redisFree() 释放。放进 unique_ptr 时，需要提供
// 自定义 deleter，否则默认 deleter 会调用 delete，导致释放方式错误。
struct RedisContextDeleter {
    void operator()(redisContext* context) const
    {
        if (context != nullptr) {
            redisFree(context);
        }
    }
};

// hiredis 的 redisReply 由 freeReplyObject() 释放，同样不能直接 delete。
struct RedisReplyDeleter {
    void operator()(redisReply* reply) const
    {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};

// unique_ptr 的第二个模板参数是删除器类型。这样函数中只要创建 RedisContextPtr
// 或 RedisReplyPtr，就能在离开作用域时自动释放 hiredis 资源。
using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

/**
 * @brief 将毫秒超时转换为 hiredis 需要的 timeval。
 */
timeval to_timeval(int milliseconds)
{
    milliseconds = milliseconds > 0 ? milliseconds : 20;
    timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    return tv;
}
}

/**
 * @brief 构造 Redis 缓存客户端配置。
 *
 * 当前 RedisCache 不持有长连接，只保存连接参数。每次 get/put/erase 单独建连，
 * 简化多线程安全问题；后续如果需要更高性能，可以在这个类内部替换为连接池。
 */
RedisCache::RedisCache(std::string host,
                       int port,
                       int db,
                       int connectTimeoutMs,
                       int commandTimeoutMs)
    : host_(std::move(host))
    , port_(port > 0 ? port : 6379)
    , db_(db >= 0 ? db : 0)
    , connectTimeoutMs_(connectTimeoutMs > 0 ? connectTimeoutMs : 20)
    , commandTimeoutMs_(commandTimeoutMs > 0 ? commandTimeoutMs : 20)
{
}

/**
 * @brief 从 Redis 读取 key。
 */
bool RedisCache::get(const std::string& key, std::string& value)
{
    // 每次命令创建独立连接，RedisContextPtr 保证本函数结束时自动 redisFree。
    RedisContextPtr context(connect());
    if (context == nullptr || !select_db(context.get())) {
        return false;
    }

    // hiredis 的 redisCommand 支持 printf 风格格式串。%b 表示“二进制安全字符串”，
    // 后面必须传 data 指针和长度，适合保存 JSON 等可能包含空字节以外任意内容的值。
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(), "GET %b", key.data(), key.size())));
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        return false;
    }

    // redisReply 的 str 不保证按 C 字符串处理安全，使用 len 构造 std::string。
    value.assign(reply->str, reply->len);
    return true;
}

/**
 * @brief 写入 Redis。
 */
void RedisCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    RedisContextPtr context(connect());
    if (context == nullptr || !select_db(context.get())) {
        return;
    }

    if (ttlSeconds > 0) {
        // SETEX key seconds value：写入并设置过期时间。key 和 value 都用 %b，
        // 避免因为字符串中出现特殊字符而被 hiredis 按普通 C 字符串截断。
        RedisReplyPtr reply(static_cast<redisReply*>(
            redisCommand(context.get(),
                         "SETEX %b %d %b",
                         key.data(),
                         key.size(),
                         ttlSeconds,
                         value.data(),
                         value.size())));
    } else {
        // ttlSeconds <= 0 与本项目 Cache 接口约定一致，表示不过期，因此使用 SET。
        RedisReplyPtr reply(static_cast<redisReply*>(
            redisCommand(context.get(),
                         "SET %b %b",
                         key.data(),
                         key.size(),
                         value.data(),
                         value.size())));
    }
}

/**
 * @brief 删除 Redis 中的 key。
 */
void RedisCache::erase(const std::string& key)
{
    RedisContextPtr context(connect());
    if (context == nullptr || !select_db(context.get())) {
        return;
    }

    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(), "DEL %b", key.data(), key.size())));
}

/**
 * @brief 建立 Redis 连接并设置命令超时。
 *
 * 返回裸指针是为了直接交给 RedisContextPtr 接管。失败时返回 nullptr，调用方将其
 * 视为缓存不可用，不影响主搜索流程。
 */
redisContext* RedisCache::connect() const
{
    timeval connectTimeout = to_timeval(connectTimeoutMs_);
    redisContext* context = redisConnectWithTimeout(host_.c_str(), port_, connectTimeout);
    if (context == nullptr) {
        return nullptr;
    }
    if (context->err != 0) {
        redisFree(context);
        return nullptr;
    }

    // redisSetTimeout 设置后续命令的读写超时，避免 Redis 卡住时阻塞业务线程太久。
    timeval commandTimeout = to_timeval(commandTimeoutMs_);
    if (redisSetTimeout(context, commandTimeout) != REDIS_OK) {
        redisFree(context);
        return nullptr;
    }

    return context;
}

/**
 * @brief 切换 Redis DB。
 */
bool RedisCache::select_db(redisContext* context) const
{
    if (context == nullptr || db_ == 0) {
        return context != nullptr;
    }

    // SELECT 失败时返回 false，外层会把本次 Redis 操作当作失败处理。
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context, "SELECT %d", db_)));
    return reply != nullptr && reply->type != REDIS_REPLY_ERROR;
}
