#include "../../include/cache/RedisCache.h"

#include <hiredis/hiredis.h>

#include <chrono>
#include <memory>
#include <sys/time.h>
#include <utility>

namespace
{
// hiredis 的 redisReply 由 freeReplyObject() 释放，同样不能直接 delete。
struct RedisReplyDeleter {
    void operator()(redisReply* reply) const
    {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};

// unique_ptr 使用自定义删除器，保证函数离开作用域时自动释放 reply。
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
 * 连接池采用惰性建连：只有首次并发访问需要时才创建连接，避免服务启动阶段因
 * Redis 暂时不可用而失败。连接由池独占借出，因此 hiredis context 不会并发使用。
 */
RedisCache::RedisCache(std::string host,
                       int port,
                       int db,
                       int connectTimeoutMs,
                       int commandTimeoutMs,
                       int poolSize,
                       int poolWaitTimeoutMs)
    : host_(std::move(host))
    , port_(port > 0 ? port : 6379)
    , db_(db >= 0 ? db : 0)
    , connectTimeoutMs_(connectTimeoutMs > 0 ? connectTimeoutMs : 20)
    , commandTimeoutMs_(commandTimeoutMs > 0 ? commandTimeoutMs : 20)
    , poolSize_(static_cast<std::size_t>(poolSize > 0 ? poolSize : 16))
    , poolWaitTimeoutMs_(poolWaitTimeoutMs > 0 ? poolWaitTimeoutMs : 20)
{
}

RedisCache::~RedisCache()
{
    std::lock_guard<std::mutex> lock(poolMutex_);
    for (redisContext* context : idleConnections_) {
        redisFree(context);
    }
    idleConnections_.clear();
    totalConnections_ = 0;
}

/**
 * @brief 从 Redis 读取 key。
 */
bool RedisCache::get(const std::string& key, std::string& value)
{
    redisContext* context = acquire();
    if (context == nullptr) {
        return false;
    }

    // hiredis 的 redisCommand 支持 printf 风格格式串。%b 表示“二进制安全字符串”，
    // 后面必须传 data 指针和长度，适合保存 JSON 等可能包含空字节以外任意内容的值。
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context, "GET %b", key.data(), key.size())));
    const bool healthy = reply != nullptr && context->err == 0;
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        release(context, healthy);
        return false;
    }

    // redisReply 的 str 不保证按 C 字符串处理安全，使用 len 构造 std::string。
    value.assign(reply->str, reply->len);
    release(context, healthy);
    return true;
}

/**
 * @brief 写入 Redis。
 */
void RedisCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    redisContext* context = acquire();
    if (context == nullptr) {
        return;
    }

    RedisReplyPtr reply;
    if (ttlSeconds > 0) {
        // SETEX key seconds value：写入并设置过期时间。key 和 value 都用 %b，
        // 避免因为字符串中出现特殊字符而被 hiredis 按普通 C 字符串截断。
        reply.reset(static_cast<redisReply*>(
            redisCommand(context,
                         "SETEX %b %d %b",
                         key.data(), key.size(), ttlSeconds,
                         value.data(), value.size())));
    } else {
        // ttlSeconds <= 0 与本项目 Cache 接口约定一致，表示不过期，因此使用 SET。
        reply.reset(static_cast<redisReply*>(
            redisCommand(context,
                         "SET %b %b",
                         key.data(), key.size(), value.data(), value.size())));
    }
    release(context, reply != nullptr && context->err == 0);
}

/**
 * @brief 删除 Redis 中的 key。
 */
void RedisCache::erase(const std::string& key)
{
    redisContext* context = acquire();
    if (context == nullptr) {
        return;
    }

    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context, "DEL %b", key.data(), key.size())));
    release(context, reply != nullptr && context->err == 0);
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

    if (!select_db(context)) {
        redisFree(context);
        return nullptr;
    }

    return context;
}

redisContext* RedisCache::acquire()
{
    std::unique_lock<std::mutex> lock(poolMutex_);
    const auto ready = [this]() {
        return !idleConnections_.empty() || totalConnections_ < poolSize_;
    };
    if (!poolCondition_.wait_for(lock,
                                 std::chrono::milliseconds(poolWaitTimeoutMs_),
                                 ready)) {
        return nullptr;
    }

    if (!idleConnections_.empty()) {
        redisContext* context = idleConnections_.back();
        idleConnections_.pop_back();
        return context;
    }

    // 先预留容量，再在锁外执行可能阻塞的 TCP 建连。其他线程可以并行创建剩余连接，
    // 但 totalConnections_ 的预留保证连接总数不会超过 poolSize_。
    ++totalConnections_;
    lock.unlock();
    redisContext* context = connect();
    if (context != nullptr) {
        return context;
    }

    lock.lock();
    --totalConnections_;
    lock.unlock();
    poolCondition_.notify_one();
    return nullptr;
}

void RedisCache::release(redisContext* context, bool healthy)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(poolMutex_);
    if (healthy && context->err == 0) {
        idleConnections_.push_back(context);
    } else {
        redisFree(context);
        --totalConnections_;
    }
    poolCondition_.notify_one();
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
