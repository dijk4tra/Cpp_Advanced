#include "../../include/online/WebSearcher.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

/**
 * @brief 构造网页搜索器并初始化 cppjieba。
 */
WebSearcher::WebSearcher()
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
    stopWords_ = TextUtils::load_stop_words(stopWords);
    pageLibrary_.load(pages, offsets);
    load_inverted_index(invertIndex);
}

/**
 * @brief 设置动态摘要长度。
 */
void WebSearcher::set_abstract_length(int length)
{
    abstractLength_ = length > 0 ? length : 150;
}

/**
 * @brief 执行网页搜索并返回 JSON。
 */
std::string WebSearcher::search_json(const std::string& query, int topK) const
{
    topK = topK <= 0 ? 10 : topK;

    nlohmann::json response;
    response["type"] = "web";
    response["query"] = query;
    response["results"] = nlohmann::json::array();

    std::map<std::string, int> termCount = cut_query(query);
    if (termCount.empty()) {
        // 查询被分词和过滤后没有有效词，返回空结果。
        return response.dump();
    }

    std::map<std::string, double> queryVector = build_query_vector(termCount);
    if (queryVector.empty()) {
        // 任一查询词不在倒排索引中，或向量无法归一化时都没有候选结果。
        return response.dump();
    }

    std::set<int> candidateDocs = find_candidate_docs(queryVector);
    if (candidateDocs.empty()) {
        return response.dump();
    }

    struct SearchResult {
        int docId = 0;
        double score = 0.0;
    };

    std::vector<SearchResult> results;
    for (int docId : candidateDocs) {
        double score = 0.0;

        // 文档向量在一期已经按 L2 范数归一化。查询向量在 build_query_vector()
        // 中也已归一化，因此余弦相似度可直接按点积计算。
        for (const auto& [word, queryWeight] : queryVector) {
            auto wordIt = invertedIndex_.find(word);
            if (wordIt == invertedIndex_.end()) {
                continue;
            }
            auto docIt = wordIt->second.find(docId);
            if (docIt != wordIt->second.end()) {
                score += queryWeight * docIt->second;
            }
        }

        results.push_back({docId, score});
    }

    // 分数高的排在前面；分数相同按 docId 升序，保证结果稳定。
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

    int count = std::min(topK, static_cast<int>(results.size()));
    for (int i = 0; i < count; ++i) {
        const Document* doc = pageLibrary_.find(results[i].docId);
        if (doc == nullptr) {
            continue;
        }

        response["results"].push_back({
            {"id", doc->id},
            {"title", doc->title},
            {"link", doc->link},
            // 摘要在查询时动态生成，因此可以围绕本次关键词截取最相关片段。
            {"abstract", DynamicAbstract::generate(doc->content, keywords, abstractLength_)},
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
        while (iss >> docId >> weight) {
            // 离线阶段已经对文档 TF-IDF 向量做过 L2 归一化，weight 可直接用于点积。
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
    for (const auto& [word, count] : termCount) {
        totalWords += count;
    }

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
        double tf = static_cast<double>(count) / totalWords;
        double df = static_cast<double>(it->second.size());
        double idf = std::log2(documentCount / (df + 1.0));
        double weight = tf * idf;
        queryVector[word] = weight;
        squareSum += weight * weight;
    }

    double norm = std::sqrt(squareSum);
    if (norm == 0.0) {
        return {};
    }

    // 归一化后，查询向量和文档向量的余弦相似度可直接用点积计算。
    for (auto& [word, weight] : queryVector) {
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
