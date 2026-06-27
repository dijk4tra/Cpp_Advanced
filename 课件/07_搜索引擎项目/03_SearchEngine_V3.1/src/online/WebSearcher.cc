#include "../../include/online/WebSearcher.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
/**
 * @brief 将 Document 序列化为 JSON 字符串。
 *
 * 文档缓存只需要保存展示需要的字段。使用 JSON 而不是手写分隔符，可以避免正文
 * 中本身包含换行、冒号等字符时产生解析歧义。
 */
std::string serialize_document(const Document& doc)
{
    nlohmann::json json;
    json["id"] = doc.id;
    json["title"] = doc.title;
    json["link"] = doc.link;
    json["content"] = doc.content;
    return json.dump();
}

/**
 * @brief 从文档缓存中的 JSON 字符串还原 Document。
 *
 * Redis 或本地缓存中可能存在旧版本/损坏数据，因此解析失败时返回 false，
 * 调用方会退回到 PageLibrary 按需读取。
 */
bool deserialize_document(const std::string& text, Document& doc)
{
    try {
        nlohmann::json json = nlohmann::json::parse(text);
        doc.id = json.value("id", 0);
        doc.title = json.value("title", "");
        doc.link = json.value("link", "");
        doc.content = json.value("content", "");
        return doc.id != 0;
    } catch (const std::exception&) {
        return false;
    }
}
}

/**
 * @brief 构造网页搜索器并初始化 cppjieba。
 */
WebSearcher::WebSearcher()
    // tokenizer_ 是 cppjieba::Jieba 对象，构造时会加载分词词典。
    // 放在成员初始化列表中，保证对象创建时分词器就已经可用。
    : tokenizer_()
{
}

/**
 * @brief 加载网页搜索依赖的离线数据。
 */
void WebSearcher::load(const std::string& pages,
                       const std::string& offsets,
                       const std::string& invertIndex,
                       const std::string& docStats,
                       const std::string& stopWords)
{
    // 加载顺序：停用词用于查询分词过滤；网页库用于结果展示；文档统计和倒排
    // 索引用于 BM25 召回与打分。四者都在服务启动阶段一次性加载完成。
    // 启动阶段加载失败直接抛异常，避免服务器带着不完整索引继续运行。
    stopWords_ = TextUtils::load_stop_words(stopWords);
    pageLibrary_.load(pages, offsets);
    load_document_stats(docStats);

    if (documentCount_ != static_cast<int>(pageLibrary_.size())) {
        throw std::runtime_error("BM25 stats document count does not match page library");
    }
    load_inverted_index(invertIndex);
}

void WebSearcher::set_bm25_parameters(double k1, double b)
{
    bm25K1_ = k1 > 0.0 ? k1 : 1.5;
    bm25B_ = b >= 0.0 && b <= 1.0 ? b : 0.75;
}

/**
 * @brief 设置动态摘要长度。
 */
void WebSearcher::set_abstract_length(int length)
{
    // 摘要长度必须为正数；非法配置统一回退到 150，保持结果可读。
    abstractLength_ = length > 0 ? length : 150;
}

/**
 * @brief 设置文档和动态摘要缓存。
 */
void WebSearcher::set_detail_cache(Cache* cache,
                                   std::string cacheVersion,
                                   int documentTtlSeconds,
                                   int abstractTtlSeconds)
{
    // WebSearcher 不拥有 cache，只保存指针。cache 的实际生命周期由 online_main 中的
    // unique_ptr 管理，并覆盖 WebSearcher 的整个使用周期。
    detailCache_ = cache;
    // 缓存版本为空时使用 default，避免生成形如 "v::doc:1" 的 key。
    cacheVersion_ = cacheVersion.empty() ? "default" : std::move(cacheVersion);
    // TTL 原样保存。具体 ttlSeconds <= 0 的“永不过期”语义由 Cache 实现处理。
    documentCacheTtlSeconds_ = documentTtlSeconds;
    abstractCacheTtlSeconds_ = abstractTtlSeconds;
}

/**
 * @brief 执行网页搜索并返回 JSON。
 */
std::string WebSearcher::search_json(const std::string& query, int topK) const
{
    topK = topK <= 0 ? 33 : topK;

    nlohmann::json response;
    // 即使没有搜索结果，也返回固定 JSON 结构，前端可以统一读取 results 数组。
    response["type"] = "web";
    response["query"] = query;
    response["results"] = nlohmann::json::array();

    // 第一步：分词并统计查询词频，得到查询 TF 的基础数据。
    std::map<std::string, int> termCount = cut_query(query);
    if (termCount.empty()) {
        // 查询被分词和过滤后没有有效词，返回空结果。
        return response.dump();
    }

    // 第二步：忽略 OOV，保留倒排索引中存在的查询词。
    std::vector<std::string> queryTerms = find_valid_query_terms(termCount);
    if (queryTerms.empty()) {
        // 查询中没有任何已登录词时没有候选结果。
        return response.dump();
    }

    // 第三步：取所有有效查询词 posting list 的并集，得到 OR 召回候选集合。
    std::set<int> candidateDocs = find_candidate_docs(queryTerms);
    if (candidateDocs.empty()) {
        return response.dump();
    }

    // 局部结构体只保存排序需要的字段，真正展示信息稍后再从 PageLibrary 查询。
    struct SearchResult {
        int docId = 0;
        double score = 0.0;
    };

    std::vector<SearchResult> results;
    for (int docId : candidateDocs) {
        double score = 0.0;

        // 每个有效查询词分别贡献 BM25 分数。未出现在当前文档中的词贡献 0；
        // 同时命中更多词的文档通常会累积更高分数。
        for (const auto& term : queryTerms) {
            score += bm25_term_score(term, docId);
        }

        // 使用列表初始化构造 SearchResult。
        results.push_back({docId, score});
    }

    // 分数高的排在前面；分数相同按 docId 升序，保证结果稳定。
    // sort 只改变 vector 中元素顺序，不改变 SearchResult 内容。
    std::sort(results.begin(), results.end(), [](const SearchResult& lhs, const SearchResult& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.docId < rhs.docId;
    });

    // topK 可能大于实际结果数，取二者较小值避免数组越界。
    int count = std::min(topK, static_cast<int>(results.size()));
    for (int i = 0; i < count; ++i) {
        Document doc;
        if (!get_document(results[i].docId, doc)) {
            continue;
        }

        // initializer list 构造一个 JSON object，再追加到 results 数组中。
        response["results"].push_back({
            {"id", doc.id},
            {"title", doc.title},
            {"link", doc.link},
            // 摘要在查询时动态生成，因此可以围绕本次关键词截取最相关片段。
            {"abstract", get_abstract(doc, queryTerms)},
            {"score", results[i].score}
        });
    }

    return response.dump();
}

/**
 * @brief 加载倒排索引。
 */
void WebSearcher::load_inverted_index(const std::string& filename)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open inverted index: " + filename);
    }

    invertedIndex_.clear();

    std::string line;
    // 倒排索引每一行长度不固定，getline 先拿到整行，再用 istringstream 逐项解析。
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string word;
        // 每行格式：word df docId tf docId tf ...
        std::size_t expectedDf = 0;
        iss >> word >> expectedDf;
        if (word.empty() || expectedDf == 0) {
            continue;
        }
        if (expectedDf > static_cast<std::size_t>(documentCount_)) {
            throw std::runtime_error("BM25 posting df exceeds document count for term: " + word);
        }

        int docId = 0;
        int tf = 0;
        std::size_t actualDf = 0;
        while (iss >> docId >> tf) {
            if (docId <= 0 || docId > documentCount_ || tf <= 0) {
                throw std::runtime_error("invalid BM25 posting in: " + filename);
            }
            invertedIndex_[word][docId] = tf;
            ++actualDf;
        }
        if (actualDf != expectedDf || invertedIndex_[word].size() != expectedDf) {
            throw std::runtime_error("BM25 posting df mismatch for term: " + word);
        }
    }
}

void WebSearcher::load_document_stats(const std::string& filename)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open BM25 document stats: " + filename);
    }

    std::string magic;
    if (!(ifs >> magic >> documentCount_ >> averageDocumentLength_)
        || magic != "BM25_STATS_V1"
        || documentCount_ <= 0
        || averageDocumentLength_ <= 0.0) {
        throw std::runtime_error("invalid BM25 document stats header: " + filename);
    }

    documentLengths_.clear();
    int docId = 0;
    int documentLength = 0;
    while (ifs >> docId >> documentLength) {
        if (docId <= 0 || documentLength < 0) {
            throw std::runtime_error("invalid BM25 document length in: " + filename);
        }
        documentLengths_[docId] = documentLength;
    }
    if (documentLengths_.size() != static_cast<std::size_t>(documentCount_)) {
        throw std::runtime_error("BM25 document length count mismatch: " + filename);
    }
}

/**
 * @brief 对查询语句分词并统计有效词频。
 */
std::map<std::string, int> WebSearcher::cut_query(const std::string& query) const
{
    std::vector<std::string> words;
    // cppjieba 将中文查询切成词语，结果写入 words 输出参数。
    tokenizer_.Cut(query, words);

    std::map<std::string, int> termCount;
    for (const auto& word : words) {
        // 停用词和纯标点/数字等无意义 token 不参与检索。
        if (stopWords_.count(word) != 0) {
            continue;
        }
        if (TextUtils::is_useless_token(word)) {
            continue;
        }
        // map 的 operator[] 在 word 不存在时会插入默认值 0，然后 ++ 变成 1。
        ++termCount[word];
    }

    return termCount;
}

/**
 * @brief 返回倒排索引中存在的查询词，忽略 OOV。
 */
std::vector<std::string> WebSearcher::find_valid_query_terms(
    const std::map<std::string, int>& termCount) const
{
    std::vector<std::string> terms;
    terms.reserve(termCount.size());
    for (const auto& [word, count] : termCount) {
        if (invertedIndex_.find(word) != invertedIndex_.end()) {
            terms.push_back(word);
        }
    }
    return terms;
}

/**
 * @brief 取所有有效查询词 posting list 的并集。
 */
std::set<int> WebSearcher::find_candidate_docs(
    const std::vector<std::string>& queryTerms) const
{
    std::set<int> candidates;

    for (const auto& word : queryTerms) {
        auto wordIt = invertedIndex_.find(word);
        if (wordIt == invertedIndex_.end()) {
            // queryTerms 正常情况下只包含已登录词。保留该检查以容忍不一致数据，
            // 单个异常词不应让已经召回的候选文档全部失效。
            continue;
        }

        for (const auto& [docId, docWeight] : wordIt->second) {
            // OR 召回：当前词 posting list 中的每篇文档都加入候选集合。set 同时
            // 完成跨词去重；文档命中多少查询词由后续点积得分自然体现。
            candidates.insert(docId);
        }
    }

    return candidates;
}

double WebSearcher::bm25_term_score(const std::string& term, int docId) const
{
    auto termIt = invertedIndex_.find(term);
    if (termIt == invertedIndex_.end()) {
        return 0.0;
    }
    auto postingIt = termIt->second.find(docId);
    if (postingIt == termIt->second.end()) {
        return 0.0;
    }
    auto lengthIt = documentLengths_.find(docId);
    if (lengthIt == documentLengths_.end()) {
        return 0.0;
    }

    const double tf = static_cast<double>(postingIt->second);
    const double df = static_cast<double>(termIt->second.size());
    const double n = static_cast<double>(documentCount_);
    const double dl = static_cast<double>(lengthIt->second);

    // Robertson/Sparck Jones IDF 的正值平滑形式，避免高 DF 词产生负分。
    const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
    const double lengthNormalization =
        1.0 - bm25B_ + bm25B_ * dl / averageDocumentLength_;
    const double denominator = tf + bm25K1_ * lengthNormalization;
    return idf * tf * (bm25K1_ + 1.0) / denominator;
}

bool WebSearcher::get_document(int docId, Document& doc) const
{
    if (detailCache_ != nullptr) {
        std::string cached;
        std::string key = build_document_cache_key(docId);
        if (detailCache_->get(key, cached) && deserialize_document(cached, doc)) {
            // 文档展示信息缓存命中后，不再打开 pages.dat。对于热点搜索结果，
            // 这能减少随机文件读取和 XML 解析成本。
            return true;
        }
    }

    // 缓存未命中或缓存内容解析失败时，回到按需读取流程：
    // PageLibrary 根据 offsets_ 找到 docId 的字节范围，再从 pages.dat 中读取单篇 XML。
    if (!pageLibrary_.find(docId, doc)) {
        return false;
    }

    if (detailCache_ != nullptr) {
        // 回源成功后写入缓存。这里缓存的是完整 Document JSON，后续同一 docId 的
        // 不同查询都可以复用标题、链接和正文。
        detailCache_->put(build_document_cache_key(docId),
                          serialize_document(doc),
                          documentCacheTtlSeconds_);
    }
    return true;
}

std::string WebSearcher::get_abstract(const Document& doc,
                                      const std::vector<std::string>& keywords) const
{
    // 动态摘要不仅与 docId 有关，还与本次查询关键词和摘要长度有关。
    // 因此 key 必须包含这些信息，避免不同查询复用错误摘要。
    std::string key = build_abstract_cache_key(doc.id, keywords);
    if (detailCache_ != nullptr) {
        std::string cached;
        if (detailCache_->get(key, cached)) {
            // 摘要是普通字符串，命中后直接返回即可。
            return cached;
        }
    }

    // 未命中时实时生成动态摘要。DynamicAbstract 会围绕关键词选择较相关片段。
    std::string abstract = DynamicAbstract::generate(doc.content, keywords, abstractLength_);
    if (detailCache_ != nullptr) {
        // 摘要生成成功后写入缓存，后续相同 docId + keywords + length 可以直接复用。
        detailCache_->put(key, abstract, abstractCacheTtlSeconds_);
    }
    return abstract;
}

std::string WebSearcher::build_document_cache_key(int docId) const
{
    std::ostringstream oss;
    // 文档内容来自离线网页库。cacheVersion_ 变化时，旧网页库对应的文档缓存自然失效。
    oss << "v:" << cacheVersion_ << ":doc:" << docId;
    return oss.str();
}

std::string WebSearcher::build_abstract_cache_key(int docId,
                                                  const std::vector<std::string>& keywords) const
{
    std::ostringstream oss;
    // 摘要 key 由版本、业务类型、docId、摘要长度和关键词序列组成。
    // keyword.size() 可以减少简单字符串拼接产生的边界歧义。
    oss << "v:" << cacheVersion_
        << ":abstract:" << docId
        << ':' << abstractLength_;
    for (const auto& keyword : keywords) {
        oss << ':' << keyword.size() << ':' << keyword;
    }
    return oss.str();
}
