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
 * 当前实际加载时为了查询方便使用 `word -> docId -> weight`，该结构保留了
 * 传统 posting 的概念，便于阅读倒排索引相关代码和文档。
 */
struct Posting
{
    // 包含该词的文档编号。
    int docId = 0;

    // 该词在对应文档中的归一化 TF-IDF 权重。
    double weight = 0.0;
};

/**
 * @brief 在线网页搜索模块。
 *
 * 该模块加载一期生成的网页库、偏移库和倒排索引。查询时把用户输入看作一篇
 * 短文档，计算查询 TF-IDF 向量，再与候选网页的归一化 TF-IDF 权重做余弦
 * 相似度排序。
 *
 * 查询词 IDF 按本项目第二期约定从 posting list 长度推导 DF，再使用
 * `IDF = log2(N / (DF + 1))` 计算。N 为网页库文档总数。
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
     * @param stopWords 中文停用词路径。
     * @throws std::runtime_error 任一数据文件无法打开时抛出。
     */
    void load(const std::string& pages,
              const std::string& offsets,
              const std::string& invertIndex,
              const std::string& stopWords);

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
    // docId -> normalized TF-IDF weight。倒排索引每个词对应一个 posting map。
    using PostingMap = std::unordered_map<int, double>;

    /**
     * @brief 加载 invert_index.dat。
     * @param filename 倒排索引路径，每行格式为 `word docId weight ...`。
     * @throws std::runtime_error 文件无法打开时抛出。
     */
    void load_inverted_index(const std::string& filename);

    /**
     * @brief 使用 cppjieba 对查询语句分词并统计词频。
     * @param query 用户原始查询语句。
     * @return word -> 查询中出现次数。
     */
    std::map<std::string, int> cut_query(const std::string& query) const;

    /**
     * @brief 根据查询词频计算归一化查询向量。
     *
     * 若任一查询词不在倒排索引中，说明不存在同时包含全部查询词的网页，直接
     * 返回空向量。
     */
    std::map<std::string, double> build_query_vector(const std::map<std::string, int>& termCount) const;

    /**
     * @brief 取所有查询词 posting list 的交集作为候选文档。
     */
    std::set<int> find_candidate_docs(const std::map<std::string, double>& queryVector) const;

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

    // word -> docId -> normalized TF-IDF weight。
    std::unordered_map<std::string, PostingMap> invertedIndex_;

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
