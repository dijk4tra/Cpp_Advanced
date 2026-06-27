#include "../../include/online/WebSearcher.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
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
                       const std::string& stopWords)
{
    // 加载顺序：停用词用于查询分词过滤；网页库用于结果展示；倒排索引用于召回
    // 和打分。三者都在服务启动阶段一次性加载完成。
    // 启动阶段加载失败直接抛异常，避免服务器带着不完整索引继续运行。
    stopWords_ = TextUtils::load_stop_words(stopWords);
    pageLibrary_.load(pages, offsets);
    load_inverted_index(invertIndex);
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

    // 第二步：把查询词频转成归一化 TF-IDF 向量。
    std::map<std::string, double> queryVector = build_query_vector(termCount);
    if (queryVector.empty()) {
        // 任一查询词不在倒排索引中，或向量无法归一化时都没有候选结果。
        return response.dump();
    }

    // 第三步：取所有查询词 posting list 的交集，得到候选文档集合。
    std::set<int> candidateDocs = find_candidate_docs(queryVector);
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

        // 文档向量在一期已经按 L2 范数归一化。查询向量在 build_query_vector()
        // 中也已归一化，因此余弦相似度可直接按点积计算。
        // structured binding 写法 `const auto& [word, queryWeight]` 可以把 map 的
        // key/value 拆成两个变量名，代码比 first/second 更直观。
        for (const auto& [word, queryWeight] : queryVector) {
            auto wordIt = invertedIndex_.find(word);
            if (wordIt == invertedIndex_.end()) {
                continue;
            }
            auto docIt = wordIt->second.find(docId);
            if (docIt != wordIt->second.end()) {
                // 点积公式：score += 查询词权重 * 文档中该词权重。
                score += queryWeight * docIt->second;
            }
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

    std::vector<std::string> keywords;
    // 动态摘要需要知道本次查询有哪些有效关键词，用于窗口打分和高亮。
    for (const auto& [word, weight] : queryVector) {
        keywords.push_back(word);
    }

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
            {"abstract", get_abstract(doc, keywords)},
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
        // 每行格式：word docId weight docId weight ...
        iss >> word;
        if (word.empty()) {
            continue;
        }

        int docId = 0;
        double weight = 0.0;
        // 一行中可能有多个 docId/weight 对，循环读到本行结束为止。
        while (iss >> docId >> weight) {
            // 离线阶段已经对文档 TF-IDF 向量做过 L2 归一化，weight 可直接用于点积。
            // invertedIndex_[word] 不存在时会自动创建一个空 PostingMap。
            invertedIndex_[word][docId] = weight;
        }
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
 * @brief 根据查询词频计算归一化 TF-IDF 查询向量。
 */
std::map<std::string, double> WebSearcher::build_query_vector(const std::map<std::string, int>& termCount) const
{
    std::map<std::string, double> queryVector;
    double squareSum = 0.0;
    int totalWords = 0;
    // 先统计查询中的有效词总数，用于计算 TF。
    for (const auto& [word, count] : termCount) {
        totalWords += count;
    }

    // pageLibrary_.size() 是 size_t，转成 double 后参与 IDF 的浮点计算。
    const double documentCount = static_cast<double>(pageLibrary_.size());
    if (totalWords == 0 || documentCount == 0.0) {
        return queryVector;
    }

    for (const auto& [word, count] : termCount) {
        auto it = invertedIndex_.find(word);
        if (it == invertedIndex_.end()) {
            // PDF 主流程要求查询包含所有关键词的网页。任一词不在倒排索引中，
            // 后续一定没有同时包含所有词的文档，直接返回空向量。
            return {};
        }

        // 查询 TF 使用词频 / 查询有效词总数。DF 由 posting list 长度推导，
        // IDF 按第二期要求使用 log2(N / (DF + 1))。
        // count 和 totalWords 都是整数，需要转成 double，否则会发生整数除法。
        double tf = static_cast<double>(count) / totalWords;
        double df = static_cast<double>(it->second.size());
        double idf = std::log2(documentCount / (df + 1.0));
        double weight = tf * idf;
        queryVector[word] = weight;
        // squareSum 是向量每个维度权重平方和，后面开方得到 L2 范数。
        squareSum += weight * weight;
    }

    double norm = std::sqrt(squareSum);
    if (norm == 0.0) {
        return {};
    }

    // 归一化后，查询向量和文档向量的余弦相似度可直接用点积计算。
    for (auto& [word, weight] : queryVector) {
        // auto& 允许直接修改 map 中保存的 weight。
        weight /= norm;
    }
    return queryVector;
}

/**
 * @brief 取所有查询词 posting list 的交集。
 */
std::set<int> WebSearcher::find_candidate_docs(const std::map<std::string, double>& queryVector) const
{
    std::set<int> candidates;
    bool firstWord = true;

    for (const auto& [word, weight] : queryVector) {
        auto wordIt = invertedIndex_.find(word);
        if (wordIt == invertedIndex_.end()) {
            return {};
        }

        std::set<int> currentDocs;
        for (const auto& [docId, docWeight] : wordIt->second) {
            // 当前词的 posting map 中，每个 key 就是一篇包含该词的文档。
            currentDocs.insert(docId);
        }

        if (firstWord) {
            // 第一个词的 posting list 作为初始候选集合。
            candidates.swap(currentDocs);
            firstWord = false;
            continue;
        }

        // 后续每个词都与当前候选集合求交集，最终只保留同时包含所有查询词的文档。
        std::set<int> intersection;
        // set_intersection 要求输入区间有序。std::set 天然有序，正好满足要求。
        // inserter 会把交集结果不断插入 intersection。
        std::set_intersection(candidates.begin(), candidates.end(),
                              currentDocs.begin(), currentDocs.end(),
                              std::inserter(intersection, intersection.begin()));
        candidates.swap(intersection);
        if (candidates.empty()) {
            return candidates;
        }
    }

    return candidates;
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
