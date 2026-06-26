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
    std::string build_suggest_key(const std::string& query,
                                  const std::string& lang,
                                  int topK) const;
    std::string build_search_key(const std::string& query,
                                 int topK) const;
    int ttl_for_result(const std::string& jsonText) const;
    int jittered_ttl(int baseTtlSeconds) const;
    std::string get_or_compute(const std::string& key,
                               const std::function<std::string()>& compute,
                               bool suggestRequest);

    static std::string normalize_query(const std::string& query);
    static std::string normalize_lang(const std::string& query, const std::string& lang);
    static bool is_empty_result(const std::string& jsonText);
    void log_stats_if_needed() const;

private:
    struct InFlight {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::string value;
        std::exception_ptr error;
    };

    KeywordRecommender& recommender_;
    WebSearcher& searcher_;
    Cache* cache_;
    std::string cacheVersion_;
    int ttlSeconds_;
    int emptyTtlSeconds_;
    int abstractLength_;
    int statsLogInterval_;
    int ttlJitterSeconds_;

    mutable std::atomic<std::uint64_t> requestCount_{0};
    mutable std::atomic<std::uint64_t> suggestHits_{0};
    mutable std::atomic<std::uint64_t> suggestMisses_{0};
    mutable std::atomic<std::uint64_t> searchHits_{0};
    mutable std::atomic<std::uint64_t> searchMisses_{0};
    mutable std::atomic<std::uint64_t> backendComputes_{0};
    mutable std::atomic<std::uint64_t> cachePuts_{0};
    mutable std::atomic<std::uint64_t> emptyCachePuts_{0};
    mutable std::atomic<std::uint64_t> singleflightWaits_{0};

    std::mutex inFlightMutex_;
    std::unordered_map<std::string, std::shared_ptr<InFlight>> inFlight_;
};
