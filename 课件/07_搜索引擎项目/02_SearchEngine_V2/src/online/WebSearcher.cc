#include "../../include/online/WebSearcher.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

WebSearcher::WebSearcher()
    : tokenizer_()
{
}

void WebSearcher::load(const std::string& pages,
                       const std::string& offsets,
                       const std::string& invertIndex,
                       const std::string& stopWords)
{
    stopWords_ = TextUtils::load_stop_words(stopWords);
    pageLibrary_.load(pages, offsets);
    load_inverted_index(invertIndex);
}

void WebSearcher::set_abstract_length(int length)
{
    abstractLength_ = length > 0 ? length : 150;
}

std::string WebSearcher::search_json(const std::string& query, int topK) const
{
    topK = topK <= 0 ? 10 : topK;

    nlohmann::json response;
    response["type"] = "web";
    response["query"] = query;
    response["results"] = nlohmann::json::array();

    std::map<std::string, int> termCount = cut_query(query);
    if (termCount.empty()) {
        return response.dump();
    }

    std::map<std::string, double> queryVector = build_query_vector(termCount);
    if (queryVector.empty()) {
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

    std::sort(results.begin(), results.end(), [](const SearchResult& lhs, const SearchResult& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.docId < rhs.docId;
    });

    std::vector<std::string> keywords;
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
            {"abstract", DynamicAbstract::generate(doc->content, keywords, abstractLength_)},
            {"score", results[i].score}
        });
    }

    return response.dump();
}

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
        iss >> word;
        if (word.empty()) {
            continue;
        }

        int docId = 0;
        double weight = 0.0;
        while (iss >> docId >> weight) {
            invertedIndex_[word][docId] = weight;
        }
    }
}

std::map<std::string, int> WebSearcher::cut_query(const std::string& query) const
{
    std::vector<std::string> words;
    tokenizer_.Cut(query, words);

    std::map<std::string, int> termCount;
    for (const auto& word : words) {
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

    for (auto& [word, weight] : queryVector) {
        weight /= norm;
    }
    return queryVector;
}

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
            candidates.swap(currentDocs);
            firstWord = false;
            continue;
        }

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
