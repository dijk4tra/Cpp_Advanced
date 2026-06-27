#include "../../include/cache/CachedSearchService.h"

#include <cctype>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <utility>

namespace
{
/**
 * @brief 去掉字符串首尾 ASCII 空白。
 *
 * 查询词归一化只处理 ASCII 空白，不主动改动中文字符和普通英文字符。这样既能
 * 合并用户输入中的多余空格，又不会破坏 UTF-8 中文内容。
 */
std::string trim_ascii_spaces(const std::string& text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(begin, end - begin);
}
}

/**
 * @brief 构造带缓存的业务服务。
 *
 * CachedSearchService 不拥有 recommender、searcher 和 cache，只保存引用/指针。
 * 它们都在 online_main 中创建，并且生命周期覆盖整个 muduo 事件循环。
 */
CachedSearchService::CachedSearchService(KeywordRecommender& recommender,
                                         WebSearcher& searcher,
                                         Cache* cache,
                                         std::string cacheVersion,
                                         int ttlSeconds,
                                         int emptyTtlSeconds,
                                         int abstractLength,
                                         int statsLogInterval,
                                         int ttlJitterSeconds)
    : recommender_(recommender)
    , searcher_(searcher)
    , cache_(cache)
    , cacheVersion_(std::move(cacheVersion))
    , ttlSeconds_(ttlSeconds)
    , emptyTtlSeconds_(emptyTtlSeconds > 0 ? emptyTtlSeconds : ttlSeconds)
    , abstractLength_(abstractLength > 0 ? abstractLength : 150)
    , statsLogInterval_(statsLogInterval)
    , ttlJitterSeconds_(ttlJitterSeconds)
{
    if (cacheVersion_.empty()) {
        // 缓存版本参与 key 构造。配置为空时给一个稳定兜底值，避免生成空版本 key。
        cacheVersion_ = "default";
    }
}

/**
 * @brief 执行关键词推荐。
 */
std::string CachedSearchService::suggest(const std::string& query,
                                         const std::string& lang,
                                         int topK)
{
    // 先规整 query，保证 "中国"、"  中国  " 这类等价输入能复用同一个缓存 key。
    std::string normalizedQuery = normalize_query(query);
    // lang 为空时根据 query 自动推断，保证缓存 key 与 KeywordRecommender 的实际语言一致。
    std::string realLang = normalize_lang(normalizedQuery, lang);
    topK = topK <= 0 ? 10 : topK;

    std::string key = build_suggest_key(normalizedQuery, realLang, topK);
    // lambda 捕获 normalizedQuery、realLang、topK 的副本。get_or_compute 只有在缓存
    // 未命中且当前线程成为 singleflight owner 时才真正执行该 lambda。
    return get_or_compute(key, [this, normalizedQuery, realLang, topK]() {
        return recommender_.recommend_json(normalizedQuery, realLang, topK);
    }, true);
}

/**
 * @brief 执行网页搜索。
 */
std::string CachedSearchService::search(const std::string& query, int topK)
{
    std::string normalizedQuery = normalize_query(query);
    topK = topK <= 0 ? 33 : topK;

    std::string key = build_search_key(normalizedQuery, topK);
    return get_or_compute(key, [this, normalizedQuery, topK]() {
        return searcher_.search_json(normalizedQuery, topK);
    }, false);
}

/**
 * @brief 查询缓存，未命中时执行回源计算。
 *
 * 该函数集中处理三件事：
 * 1. 先查缓存；
 * 2. 缓存未命中时用 singleflight 合并相同 key 的并发回源；
 * 3. 回源成功后写缓存并记录统计。
 */
std::string CachedSearchService::get_or_compute(const std::string& key,
                                                const std::function<std::string()>& compute,
                                                bool suggestRequest)
{
    std::string cached;
    if (cache_ != nullptr && cache_->get(key, cached)) {
        // 第一层快速路径：缓存已命中，直接返回 JSON。suggestRequest 用于区分统计
        // 关键词推荐命中还是网页搜索命中。
        if (suggestRequest) {
            ++suggestHits_;
        } else {
            ++searchHits_;
        }
        log_stats_if_needed();
        return cached;
    }

    // 走到这里说明第一次查缓存未命中，先记录 miss。后面成为 owner 的线程还会
    // 再查一次缓存，避免刚好被其他线程写入后仍重复回源。
    if (suggestRequest) {
        ++suggestMisses_;
    } else {
        ++searchMisses_;
    }

    std::shared_ptr<InFlight> state;
    bool owner = false;
    {
        // inFlight_ 是 key -> 正在计算状态的表。这个表本身需要一把独立互斥锁保护。
        // 临界区只做查表/插表，尽量短，不把真正耗时的搜索计算放在这把锁里。
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        auto it = inFlight_.find(key);
        if (it == inFlight_.end()) {
            // 当前 key 没有线程在回源，当前线程成为 owner，负责执行 compute()。
            state = std::make_shared<InFlight>();
            inFlight_[key] = state;
            owner = true;
        } else {
            // 已有线程正在计算相同 key，当前线程只需要等待它的结果。
            state = it->second;
        }
    }

    if (!owner) {
        ++singleflightWaits_;
        // unique_lock 可配合 condition_variable 使用。wait 会暂时释放 state->mutex，
        // 被唤醒后再重新加锁并检查条件，避免忙等浪费 CPU。
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&state]() { return state->done; });
        if (state->error) {
            // owner 回源过程中如果抛异常，等待线程看到同一个异常并继续向上抛。
            std::rethrow_exception(state->error);
        }
        log_stats_if_needed();
        return state->value;
    }

    try {
        // owner 成为计算线程后再查一次缓存。这能处理一种竞态：当前线程拿到 owner
        // 资格前，其他请求刚好已经写入缓存。
        if (cache_ != nullptr && cache_->get(key, cached)) {
            if (suggestRequest) {
                ++suggestHits_;
            } else {
                ++searchHits_;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value = cached;
        } else {
            ++backendComputes_;
            // 真正执行原业务逻辑：关键词推荐或网页搜索。这个过程可能较耗时，
            // 但不会持有 inFlightMutex_，因此不阻塞其他 key 的 singleflight 管理。
            std::string result = compute();
            if (cache_ != nullptr) {
                bool empty = is_empty_result(result);
                // ttl_for_result 会根据是否空结果选择普通 TTL 或空结果短 TTL；
                // jittered_ttl 再给 TTL 增加随机抖动，避免大量 key 同时过期。
                cache_->put(key, result, jittered_ttl(ttl_for_result(result)));
                ++cachePuts_;
                if (empty) {
                    ++emptyCachePuts_;
                }
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value = std::move(result);
        }
    } catch (...) {
        // 捕获所有异常并存入 exception_ptr，确保等待线程不会一直阻塞。
        // 后面会设置 done 并 notify_all。
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = std::current_exception();
    }

    {
        // owner 写完 value 或 error 后，把 done 置为 true。等待线程的 wait 条件
        // 看到 done 后才会继续向下执行。
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
    }
    // 唤醒所有等待相同 key 的线程。
    state->cv.notify_all();
    {
        // 该 key 的本轮回源已经结束，从 inFlight_ 删除。后续相同 key 再 miss 时，
        // 会开启新一轮 singleflight。
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        inFlight_.erase(key);
    }

    if (state->error) {
        std::rethrow_exception(state->error);
    }
    log_stats_if_needed();
    return state->value;
}

/**
 * @brief 构造关键词推荐缓存 key。
 */
std::string CachedSearchService::build_suggest_key(const std::string& query,
                                                   const std::string& lang,
                                                   int topK) const
{
    std::ostringstream oss;
    // key 中包含 query.size()，可以避免不同字段拼接后产生歧义。例如 a:bc 与
    // ab:c 这类输入如果只靠分隔符拼接，在极端情况下可读性较差。
    oss << "v:" << cacheVersion_
        << ":suggest:" << lang
        << ':' << topK
        << ':' << query.size()
        << ':' << query;
    return oss.str();
}

/**
 * @brief 构造网页搜索缓存 key。
 */
std::string CachedSearchService::build_search_key(const std::string& query, int topK) const
{
    std::ostringstream oss;
    // 网页搜索结果包含动态摘要，摘要长度会影响 JSON 内容，因此 key 必须包含
    // abstractLength_。否则修改摘要长度后可能命中旧长度的结果。
    oss << "v:" << cacheVersion_
        << ":search:" << topK
        << ':' << abstractLength_
        << ':' << query.size()
        << ':' << query;
    return oss.str();
}

/**
 * @brief 根据结果内容选择 TTL。
 */
int CachedSearchService::ttl_for_result(const std::string& jsonText) const
{
    if (is_empty_result(jsonText)) {
        // 空结果缓存时间较短，防止不存在的 query 长时间占据缓存；同时也能避免
        // 用户反复请求不存在内容时每次都触发完整搜索流程。
        return emptyTtlSeconds_;
    }
    return ttlSeconds_;
}

/**
 * @brief 给 TTL 增加随机抖动。
 */
int CachedSearchService::jittered_ttl(int baseTtlSeconds) const
{
    if (baseTtlSeconds <= 0 || ttlJitterSeconds_ <= 0) {
        return baseTtlSeconds;
    }

    // thread_local 表示每个线程有自己的随机数生成器，避免多个 muduo 工作线程
    // 共享同一个 generator 时还要额外加锁。
    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, ttlJitterSeconds_);
    return baseTtlSeconds + distribution(generator);
}

/**
 * @brief 规整查询词。
 */
std::string CachedSearchService::normalize_query(const std::string& query)
{
    std::string trimmed = trim_ascii_spaces(query);
    std::string result;
    result.reserve(trimmed.size());

    bool previousSpace = false;
    for (unsigned char ch : trimmed) {
        // ctype 函数要求参数能表示为 unsigned char。若直接传 char，遇到高位字节
        // 可能因为负值导致未定义行为。
        bool currentSpace = std::isspace(ch) != 0;
        if (currentSpace) {
            if (!previousSpace) {
                result.push_back(' ');
            }
        } else {
            result.push_back(static_cast<char>(ch));
        }
        previousSpace = currentSpace;
    }

    return result;
}

/**
 * @brief 归一化关键词推荐语言。
 */
std::string CachedSearchService::normalize_lang(const std::string& query, const std::string& lang)
{
    if (lang == "cn" || lang == "en") {
        return lang;
    }

    for (unsigned char ch : query) {
        if (ch >= 0x80) {
            // UTF-8 中文一定包含高位字节；当前项目只区分中英文，这个判断足够使用。
            return "cn";
        }
    }
    return "en";
}

/**
 * @brief 判断 JSON 响应是否是空结果。
 */
bool CachedSearchService::is_empty_result(const std::string& jsonText)
{
    try {
        nlohmann::json json = nlohmann::json::parse(jsonText);
        auto it = json.find("results");
        return it != json.end() && it->is_array() && it->empty();
    } catch (const std::exception&) {
        // 如果不是合法 JSON，不把它当作空结果，避免异常响应被使用短 TTL 误处理。
        return false;
    }
}

/**
 * @brief 按配置周期打印缓存统计。
 */
void CachedSearchService::log_stats_if_needed() const
{
    if (statsLogInterval_ <= 0) {
        return;
    }

    std::uint64_t total = requestCount_.fetch_add(1) + 1;
    if (total % static_cast<std::uint64_t>(statsLogInterval_) != 0) {
        return;
    }

    std::uint64_t suggestHits = suggestHits_.load();
    std::uint64_t suggestMisses = suggestMisses_.load();
    std::uint64_t searchHits = searchHits_.load();
    std::uint64_t searchMisses = searchMisses_.load();
    std::uint64_t hits = suggestHits + searchHits;
    std::uint64_t misses = suggestMisses + searchMisses;
    std::uint64_t seen = hits + misses;
    // static_cast<double> 避免整数除法。seen 为 0 时命中率定义为 0。
    double hitRate = seen == 0 ? 0.0 : static_cast<double>(hits) * 100.0 / seen;

    std::cout << std::fixed << std::setprecision(2)
              << "[Cache] total=" << seen
              << " hit_rate=" << hitRate << "%"
              << " suggest_hit=" << suggestHits
              << " suggest_miss=" << suggestMisses
              << " search_hit=" << searchHits
              << " search_miss=" << searchMisses
              << " backend_compute=" << backendComputes_.load()
              << " cache_put=" << cachePuts_.load()
              << " empty_put=" << emptyCachePuts_.load()
              << " singleflight_wait=" << singleflightWaits_.load()
              << std::endl;
}
