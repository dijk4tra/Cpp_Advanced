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
        cacheVersion_ = "default";
    }
}

std::string CachedSearchService::suggest(const std::string& query,
                                         const std::string& lang,
                                         int topK)
{
    std::string normalizedQuery = normalize_query(query);
    std::string realLang = normalize_lang(normalizedQuery, lang);
    topK = topK <= 0 ? 10 : topK;

    std::string key = build_suggest_key(normalizedQuery, realLang, topK);
    return get_or_compute(key, [this, normalizedQuery, realLang, topK]() {
        return recommender_.recommend_json(normalizedQuery, realLang, topK);
    }, true);
}

std::string CachedSearchService::search(const std::string& query, int topK)
{
    std::string normalizedQuery = normalize_query(query);
    topK = topK <= 0 ? 33 : topK;

    std::string key = build_search_key(normalizedQuery, topK);
    return get_or_compute(key, [this, normalizedQuery, topK]() {
        return searcher_.search_json(normalizedQuery, topK);
    }, false);
}

std::string CachedSearchService::get_or_compute(const std::string& key,
                                                const std::function<std::string()>& compute,
                                                bool suggestRequest)
{
    std::string cached;
    if (cache_ != nullptr && cache_->get(key, cached)) {
        if (suggestRequest) {
            ++suggestHits_;
        } else {
            ++searchHits_;
        }
        log_stats_if_needed();
        return cached;
    }

    if (suggestRequest) {
        ++suggestMisses_;
    } else {
        ++searchMisses_;
    }

    std::shared_ptr<InFlight> state;
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        auto it = inFlight_.find(key);
        if (it == inFlight_.end()) {
            state = std::make_shared<InFlight>();
            inFlight_[key] = state;
            owner = true;
        } else {
            state = it->second;
        }
    }

    if (!owner) {
        ++singleflightWaits_;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&state]() { return state->done; });
        if (state->error) {
            std::rethrow_exception(state->error);
        }
        log_stats_if_needed();
        return state->value;
    }

    try {
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
            std::string result = compute();
            if (cache_ != nullptr) {
                bool empty = is_empty_result(result);
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
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
    }
    state->cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        inFlight_.erase(key);
    }

    if (state->error) {
        std::rethrow_exception(state->error);
    }
    log_stats_if_needed();
    return state->value;
}

std::string CachedSearchService::build_suggest_key(const std::string& query,
                                                   const std::string& lang,
                                                   int topK) const
{
    std::ostringstream oss;
    oss << "v:" << cacheVersion_
        << ":suggest:" << lang
        << ':' << topK
        << ':' << query.size()
        << ':' << query;
    return oss.str();
}

std::string CachedSearchService::build_search_key(const std::string& query, int topK) const
{
    std::ostringstream oss;
    oss << "v:" << cacheVersion_
        << ":search:" << topK
        << ':' << abstractLength_
        << ':' << query.size()
        << ':' << query;
    return oss.str();
}

int CachedSearchService::ttl_for_result(const std::string& jsonText) const
{
    if (is_empty_result(jsonText)) {
        return emptyTtlSeconds_;
    }
    return ttlSeconds_;
}

int CachedSearchService::jittered_ttl(int baseTtlSeconds) const
{
    if (baseTtlSeconds <= 0 || ttlJitterSeconds_ <= 0) {
        return baseTtlSeconds;
    }

    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, ttlJitterSeconds_);
    return baseTtlSeconds + distribution(generator);
}

std::string CachedSearchService::normalize_query(const std::string& query)
{
    std::string trimmed = trim_ascii_spaces(query);
    std::string result;
    result.reserve(trimmed.size());

    bool previousSpace = false;
    for (unsigned char ch : trimmed) {
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

std::string CachedSearchService::normalize_lang(const std::string& query, const std::string& lang)
{
    if (lang == "cn" || lang == "en") {
        return lang;
    }

    for (unsigned char ch : query) {
        if (ch >= 0x80) {
            return "cn";
        }
    }
    return "en";
}

bool CachedSearchService::is_empty_result(const std::string& jsonText)
{
    try {
        nlohmann::json json = nlohmann::json::parse(jsonText);
        auto it = json.find("results");
        return it != json.end() && it->is_array() && it->empty();
    } catch (const std::exception&) {
        return false;
    }
}

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
