#pragma once

#include "Cache.h"
#include "../online/KeywordRecommender.h"
#include "../online/WebSearcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * @brief 带缓存的搜索业务服务层。
 *
 * 第三期第二阶段先把缓存读写集中到这一层。后续第三阶段再让 SearchServer 和
 * WebHttpServer 依赖该类，避免两个网络入口分别实现缓存逻辑。
 */
class CachedSearchService
{
public:
    /**
     * @param recommender 原关键词推荐模块，生命周期必须长于本对象。
     * @param searcher 原网页搜索模块，生命周期必须长于本对象。
     * @param cache 缓存对象，可为 nullptr；为空时直接回源计算。
     * @param cacheVersion 缓存版本，离线数据重建后应更新。
     * @param ttlSeconds 普通结果缓存秒数；小于等于 0 表示不过期。
     * @param emptyTtlSeconds 空结果缓存秒数；小于等于 0 表示使用 ttlSeconds。
     * @param abstractLength 网页搜索摘要长度，用于组成 search 缓存 key。
     * @param statsLogInterval 每处理多少次请求打印一次缓存统计；小于等于 0 表示关闭。
     * @param ttlJitterSeconds TTL 随机抖动秒数；小于等于 0 表示不抖动。
     */
    CachedSearchService(KeywordRecommender& recommender,
                        WebSearcher& searcher,
                        Cache* cache,
                        std::string cacheVersion,
                        int ttlSeconds,
                        int emptyTtlSeconds,
                        int abstractLength,
                        int statsLogInterval,
                        int ttlJitterSeconds);

    /**
     * @brief 执行关键词推荐，优先读取缓存。
     */
    std::string suggest(const std::string& query,
                        const std::string& lang,
                        int topK);

    /**
     * @brief 执行网页搜索，优先读取缓存。
     */
    std::string search(const std::string& query,
                       int topK);

private:
    /**
     * @brief 生成关键词推荐缓存 key。
     *
     * key 中包含缓存版本、业务类型、语言、topK 和 query，确保不同参数的请求
     * 不会互相命中。
     */
    std::string build_suggest_key(const std::string& query,
                                  const std::string& lang,
                                  int topK) const;

    /**
     * @brief 生成网页搜索缓存 key。
     *
     * 网页搜索结果包含动态摘要，因此 key 还会包含摘要长度，防止修改摘要长度后
     * 仍读到旧格式结果。
     */
    std::string build_search_key(const std::string& query,
                                 int topK) const;

    /**
     * @brief 根据 JSON 结果是否为空选择 TTL。
     */
    int ttl_for_result(const std::string& jsonText) const;

    /**
     * @brief 给基础 TTL 增加随机抖动，降低同一批 key 同时过期的概率。
     */
    int jittered_ttl(int baseTtlSeconds) const;

    /**
     * @brief 统一处理“读缓存 -> singleflight 回源 -> 写缓存”的主流程。
     *
     * compute 是一个回调函数，只有缓存未命中且当前线程成为 owner 时才执行。
     * suggestRequest 用来区分统计指标属于关键词推荐还是网页搜索。
     */
    std::string get_or_compute(const std::string& key,
                               const std::function<std::string()>& compute,
                               bool suggestRequest);

    /**
     * @brief 规整用户 query，主要处理首尾空白和连续空白。
     */
    static std::string normalize_query(const std::string& query);

    /**
     * @brief 规整推荐语言参数，非法或为空时根据 query 简单推断中英文。
     */
    static std::string normalize_lang(const std::string& query, const std::string& lang);

    /**
     * @brief 判断业务 JSON 是否表示空结果。
     */
    static bool is_empty_result(const std::string& jsonText);

    /**
     * @brief 按配置周期打印缓存命中、回源和 singleflight 等统计。
     */
    void log_stats_if_needed() const;

private:
    struct InFlight {
        // 每个正在回源的 key 都有自己的 mutex/cv，等待线程只等待这个 key，
        // 不影响其他 key 的请求并发执行。
        std::mutex mutex;
        std::condition_variable cv;

        // owner 回源完成后置为 true。condition_variable::wait 使用它作为条件，
        // 可防止虚假唤醒导致等待线程提前返回。
        bool done = false;

        // 回源得到的 JSON 字符串。owner 写入后，所有等待线程复用同一份结果。
        std::string value;

        // 回源过程中出现异常时保存异常对象，等待线程被唤醒后重新抛出同一异常。
        std::exception_ptr error;
    };

    // 原始关键词推荐模块。CachedSearchService 只负责加缓存，不复制词典和索引。
    KeywordRecommender& recommender_;

    // 原始网页搜索模块。网页搜索内部还会处理文档和摘要的细粒度缓存。
    WebSearcher& searcher_;

    // 统一缓存接口，可以指向 L1、L2 或 TwoLevelCache；为空时等价于关闭缓存。
    Cache* cache_;

    // 缓存版本号。离线数据重新生成后调整该值，可自然隔离旧缓存。
    std::string cacheVersion_;

    // 普通搜索结果 TTL。
    int ttlSeconds_;

    // 空结果 TTL，通常比普通结果短，避免大量不存在查询长期占用缓存。
    int emptyTtlSeconds_;

    // 动态摘要长度，参与 search key 构造。
    int abstractLength_;

    // 统计日志间隔。值小于等于 0 表示不打印。
    int statsLogInterval_;

    // TTL 随机抖动范围，单位秒。
    int ttlJitterSeconds_;

    // 以下统计值会被多个 muduo 工作线程同时更新，因此使用 atomic。
    // atomic 的自增和读取无需额外 mutex，适合这种只做计数的场景。
    mutable std::atomic<std::uint64_t> requestCount_{0};
    mutable std::atomic<std::uint64_t> suggestHits_{0};
    mutable std::atomic<std::uint64_t> suggestMisses_{0};
    mutable std::atomic<std::uint64_t> searchHits_{0};
    mutable std::atomic<std::uint64_t> searchMisses_{0};
    mutable std::atomic<std::uint64_t> backendComputes_{0};
    mutable std::atomic<std::uint64_t> cachePuts_{0};
    mutable std::atomic<std::uint64_t> emptyCachePuts_{0};
    mutable std::atomic<std::uint64_t> singleflightWaits_{0};

    // inFlight_ 记录当前正在回源的 key，用于合并相同 key 的并发请求。
    // 该 map 本身不是线程安全容器，所以所有访问都必须先持有 inFlightMutex_。
    std::mutex inFlightMutex_;
    std::unordered_map<std::string, std::shared_ptr<InFlight>> inFlight_;
};
