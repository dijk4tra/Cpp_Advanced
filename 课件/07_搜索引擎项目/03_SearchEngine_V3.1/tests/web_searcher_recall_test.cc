#include "online/WebSearcher.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace
{
std::set<int> result_ids(const std::string& jsonText)
{
    std::set<int> ids;
    const auto response = nlohmann::json::parse(jsonText);
    for (const auto& result : response.at("results")) {
        ids.insert(result.at("id").get<int>());
    }
    return ids;
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    const std::string root = SEARCH_ENGINE_TEST_SOURCE_DIR;

    WebSearcher searcher;
    searcher.load(root + "/data/index/pages.dat",
                  root + "/data/index/offsets.dat",
                  root + "/data/index/invert_index.dat",
                  root + "/data/index/bm25_doc_stats.dat",
                  root + "/data/stopwords/cn_stopwords.txt");

    constexpr int kAllResults = 1000;

    // 部分 OOV 不应让整个查询失败，也不应改变有效词“中国”的召回结果。
    const auto known = result_ids(searcher.search_json("中国", kAllResults));
    const auto partialOov = result_ids(
        searcher.search_json("中国 qwertyuiopstrictandoovtoken", kAllResults));
    require(!known.empty(), "known term should return documents");
    require(partialOov == known,
            "an OOV term must not discard documents recalled by a known term");

    // 组合查询应召回两个有效词 posting list 的并集，而不是交集。
    const auto searchEngine = result_ids(searcher.search_json("搜索引擎", kAllResults));
    const auto stock = result_ids(searcher.search_json("股票", kAllResults));
    auto expectedUnion = searchEngine;
    expectedUnion.insert(stock.begin(), stock.end());

    const std::string combinedJson = searcher.search_json("搜索引擎 股票", kAllResults);
    const auto combined = result_ids(combinedJson);
    require(!searchEngine.empty() && !stock.empty(),
            "fixture terms should both exist in the inverted index");
    require(combined == expectedUnion,
            "multi-term search should return the union of valid posting lists");

    // BM25 的平滑 IDF 和词频/长度归一化应为所有实际命中的结果生成有限正分。
    for (const auto& result : nlohmann::json::parse(combinedJson).at("results")) {
        const double score = result.at("score").get<double>();
        require(std::isfinite(score) && score > 0.0,
                "BM25 scores should be finite and positive");
    }

    std::cout << "web_searcher_recall_test passed\n";
    return 0;
}
