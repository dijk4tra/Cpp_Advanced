#include "../../include/cache/RedisCache.h"

#include <hiredis/hiredis.h>

#include <chrono>
#include <memory>
#include <spdlog/spdlog.h>
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

/** @brief 释放连接池中仍处于空闲状态的 hiredis 连接。 */
RedisCache::~RedisCache()
{
    // 析构发生在服务线程退出之后，此时不应再有借出的连接。lock_guard 使用 RAII
    // 自动加锁/解锁，保证遍历和清空池时不会与意外的并发归还操作交叉。
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
    // acquire 返回的裸指针在 release 前由当前线程独占。这里不能用一个简单的
    // unique_ptr 接管，因为健康连接需要归还池中，而不是每次调用都 redisFree。
    redisContext* context = acquire();
    if (context == nullptr) {
        log_failure("get_connection_unavailable");
        return false;
    }

    // hiredis 的 redisCommand 支持 printf 风格格式串。%b 表示“二进制安全字符串”，
    // 后面必须传 data 指针和长度，适合保存 JSON 等可能包含空字节以外任意内容的值。
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context, "GET %b", key.data(), key.size())));
    // reply 为空通常表示网络/超时错误，此时 context 协议状态可能已不同步，不能
    // 继续放回池中。REDIS_REPLY_NIL 只是 key 不存在，连接本身仍然健康。
    const bool healthy = reply != nullptr && context->err == 0;
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            log_failure("get_command_failed");
        }
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
        log_failure("put_connection_unavailable");
        return;
    }

    // 先默认构造空 unique_ptr，再根据 TTL 分支通过 reset 接管 redisCommand 返回值。
    // 两个分支离开函数时都由同一个 RAII 对象释放 reply，避免重复清理代码。
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
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        log_failure("put_command_failed");
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
        log_failure("erase_connection_unavailable");
        return;
    }

    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context, "DEL %b", key.data(), key.size())));
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        log_failure("erase_command_failed");
    }
    release(context, reply != nullptr && context->err == 0);
}

/**
 * @brief 建立 Redis 连接并设置命令超时。
 *
 * 返回裸指针是为了交给连接池管理：健康连接会反复借出/归还，不能由一次命令的
 * 局部智能指针释放。失败时返回 nullptr，调用方将其视为缓存不可用。
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

    // DB 选择属于连接状态，只在新连接创建时执行一次；后续从池中复用时无需为
    // 每条 GET/SET 再发送 SELECT，减少一次 Redis 往返。
    if (!select_db(context)) {
        redisFree(context);
        return nullptr;
    }

    return context;
}

/**
 * @brief 从连接池借出一条连接，必要时惰性创建。
 *
 * 过程分为“等待可用容量 -> 优先复用空闲连接 -> 预留槽位 -> 锁外建连”。
 * totalConnections_ 同时包含空闲和已借出连接，因此并发建连不会超过 poolSize_。
 */
redisContext* RedisCache::acquire()
{
    // unique_lock 与 lock_guard 的关键区别是可以手动 unlock/lock，并能传给
    // condition_variable::wait_for；后面的网络建连必须在锁外执行。
    std::unique_lock<std::mutex> lock(poolMutex_);
    // lambda 捕获 this，条件变量每次唤醒后都会重新检查谓词，可正确处理虚假唤醒。
    const auto ready = [this]() {
        return !idleConnections_.empty() || totalConnections_ < poolSize_;
    };
    if (!poolCondition_.wait_for(lock,
                                 std::chrono::milliseconds(poolWaitTimeoutMs_),
                                 ready)) {
        return nullptr;
    }

    if (!idleConnections_.empty()) {
        // vector 尾部 push/pop 都是均摊 O(1)，连接池不需要保持连接顺序，因此把它
        // 当作栈使用即可。刚归还的连接优先复用，也更不容易被服务端空闲超时关闭。
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

    // connect 失败时撤销之前预留的槽位并唤醒等待线程，使它可以重试建连或使用
    // 其他线程刚归还的连接。
    lock.lock();
    --totalConnections_;
    lock.unlock();
    poolCondition_.notify_one();
    return nullptr;
}

/**
 * @brief 将连接归还池中，或销毁不可继续复用的连接。
 * @param healthy 命令层根据 reply 和 context 状态给出的健康判断。
 */
void RedisCache::release(redisContext* context, bool healthy)
{
    if (context == nullptr) {
        return;
    }

    // 即使调用方认为连接健康，也再次检查 context->err，形成最后一道防线。
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
 * @brief 对 Redis 降级日志做简单采样。
 *
 * fetch_add 是原子“读取旧值并加一”；加 1 后得到当前故障序号。只记录第一次和
 * 每 100 次，既能看到故障仍在持续，又避免 Redis 宕机时每个请求都写 WARN。
 */
void RedisCache::log_failure(const char* reason)
{
    const std::uint64_t count = failureCount_.fetch_add(1) + 1;
    if (count == 1 || count % 100 == 0) {
        spdlog::warn("Redis cache degraded host={} port={} reason={} failure_count={}",
                     host_, port_, reason, count);
    }
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
