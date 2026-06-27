#pragma once

#include "DynamicAbstract.h"
#include "PageLibrary.h"
#include "../cache/Cache.h"

#include <cppjieba/Jieba.hpp>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 倒排索引中的一条 posting。
 *
 * 当前实际加载时为了查询方便使用 `word -> docId -> tf`，该结构保留了
 * 传统 posting 的概念，便于阅读倒排索引相关代码和文档。
 */
struct Posting
{
    // 包含该词的文档编号。
    int docId = 0;

    // 该词在对应文档中的原始出现次数，供在线 BM25 计算使用。
    int termFrequency = 0;
};

/**
 * @brief 在线网页搜索模块。
 *
 * 该模块加载网页库、偏移库、原始词频倒排索引和 BM25 文档长度统计。查询时
 * 对有效查询词执行 OR 召回，再根据 tf、df、dl、avgdl、N 实时计算 BM25 分数。
 */
class WebSearcher
{
public:
    /**
     * @brief 初始化分词器。
     */
    WebSearcher();

    /**
     * @brief 加载网页搜索所需数据。
     *
     * @param pages 网页库路径。
     * @param offsets 偏移库路径。
     * @param invertIndex 倒排索引路径。
     * @param docStats BM25 文档统计路径。
     * @param stopWords 中文停用词路径。
     * @throws std::runtime_error 任一数据文件无法打开时抛出。
     */
    void load(const std::string& pages,
              const std::string& offsets,
              const std::string& invertIndex,
              const std::string& docStats,
              const std::string& stopWords);

    /**
     * @brief 设置 BM25 参数。
     * @param k1 词频饱和参数，必须大于 0，非法值回退为 1.5。
     * @param b 文档长度归一化参数，范围 [0,1]，非法值回退为 0.75。
     */
    void set_bm25_parameters(double k1, double b);

    /**
     * @brief 设置动态摘要长度。
     * @param length 摘要字符数上限，非法值会回退为 150。
     */
    void set_abstract_length(int length);

    /**
     * @brief 设置文档展示信息和动态摘要片段缓存。
     * @param cache 缓存对象，可为 nullptr；为空时不启用细粒度缓存。
     * @param cacheVersion 缓存版本，用于隔离离线重建后的旧数据。
     * @param documentTtlSeconds 文档缓存 TTL。
     * @param abstractTtlSeconds 动态摘要缓存 TTL。
     */
    void set_detail_cache(Cache* cache,
                          std::string cacheVersion,
                          int documentTtlSeconds,
                          int abstractTtlSeconds);

    /**
     * @brief 搜索网页并生成 JSON 响应。
     * @param query 用户查询语句。
     * @param topK 返回结果数。
     * @return JSON 字符串。
     */
    std::string search_json(const std::string& query, int topK) const;

private:
    // docId -> raw term frequency。倒排索引每个词对应一个 posting map。
    using PostingMap = std::unordered_map<int, int>;

    /**
     * @brief 加载 invert_index.dat。
     * @param filename 倒排索引路径，每行格式为 `word df docId tf ...`。
     * @throws std::runtime_error 文件无法打开时抛出。
     */
    void load_inverted_index(const std::string& filename);

    /**
     * @brief 加载 `BM25_STATS_V1 N avgdl` 和后续 `docId dl` 记录。
     */
    void load_document_stats(const std::string& filename);

    /**
     * @brief 使用 cppjieba 对查询语句分词并统计词频。
     * @param query 用户原始查询语句。
     * @return word -> 查询中出现次数。
     */
    std::map<std::string, int> cut_query(const std::string& query) const;

    /** @brief 从查询词频中筛出存在于倒排索引的词，忽略 OOV。 */
    std::vector<std::string> find_valid_query_terms(
        const std::map<std::string, int>& termCount) const;

    /**
     * @brief 取所有有效查询词 posting list 的并集作为候选文档。
     *
     * 当前采用 OR 召回：文档命中任意一个有效查询词即可进入排序阶段，命中词
     * 的数量和权重由后续 BM25 分数反映。这样不会因为某个未登录词或缺失词
     * 把其他相关文档提前排除。
     */
    std::set<int> find_candidate_docs(const std::vector<std::string>& queryTerms) const;

    /** @brief 计算一个查询词对指定文档的 BM25 得分贡献。 */
    double bm25_term_score(const std::string& term, int docId) const;

    /**
     * @brief 获取文档，优先读取文档缓存，未命中时按需读取网页库。
     */
    bool get_document(int docId, Document& doc) const;

    /**
     * @brief 获取动态摘要，优先读取摘要缓存，未命中时实时生成。
     */
    std::string get_abstract(const Document& doc,
                             const std::vector<std::string>& keywords) const;

    std::string build_document_cache_key(int docId) const;
    std::string build_abstract_cache_key(int docId,
                                         const std::vector<std::string>& keywords) const;

private:
    // cppjieba 的初始化成本较高，作为成员在服务生命周期内复用。
    cppjieba::Jieba tokenizer_;

    // 中文停用词集合，用于过滤查询中的高频低信息量词语。
    std::set<std::string> stopWords_;

    // 网页库访问器。第三期中它只把偏移库加载到内存，正文按 docId 从 pages.dat 按需读取。
    PageLibrary pageLibrary_;

    // word -> docId -> raw term frequency。
    std::unordered_map<std::string, PostingMap> invertedIndex_;

    // docId -> 过滤后的有效 token 数（dl）。
    std::unordered_map<int, int> documentLengths_;

    // BM25 全库统计和参数。
    int documentCount_ = 0;
    double averageDocumentLength_ = 0.0;
    double bm25K1_ = 1.5;
    double bm25B_ = 0.75;

    // 动态摘要最大长度，既影响展示结果，也参与摘要缓存 key。
    int abstractLength_ = 150;

    // 文档展示信息和动态摘要共用的缓存接口。为空表示关闭细粒度缓存。
    Cache* detailCache_ = nullptr;

    // 细粒度缓存版本，离线网页库重建后应更新，避免读取旧文档/旧摘要。
    std::string cacheVersion_ = "default";

    // 文档缓存 TTL。文档展示信息只随离线数据变化，通常可以比搜索结果缓存更稳定。
    int documentCacheTtlSeconds_ = 600;

    // 摘要缓存 TTL。摘要与查询关键词相关，数量可能更多，因此单独配置。
    int abstractCacheTtlSeconds_ = 600;
};
