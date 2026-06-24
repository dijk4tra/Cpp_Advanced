#pragma once

#include "DynamicAbstract.h"
#include "PageLibrary.h"

#include <cppjieba/Jieba.hpp>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 倒排索引中的一条 posting。
 */
struct Posting
{
    int docId = 0;
    double weight = 0.0;
};

/**
 * @brief 在线网页搜索模块。
 *
 * 该模块加载一期生成的网页库、偏移库和倒排索引。查询时把用户输入看作一篇
 * 短文档，计算查询 TF-IDF 向量，再与候选网页的归一化 TF-IDF 权重做余弦
 * 相似度排序。
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
     * @brief 搜索网页并生成 JSON 响应。
     * @param query 用户查询语句。
     * @param topK 返回结果数。
     * @return JSON 字符串。
     */
    std::string search_json(const std::string& query, int topK) const;

private:
    using PostingMap = std::unordered_map<int, double>;

    void load_inverted_index(const std::string& filename);
    std::map<std::string, int> cut_query(const std::string& query) const;
    std::map<std::string, double> build_query_vector(const std::map<std::string, int>& termCount) const;
    std::set<int> find_candidate_docs(const std::map<std::string, double>& queryVector) const;

private:
    // cppjieba 的初始化成本较高，作为成员在服务生命周期内复用。
    cppjieba::Jieba tokenizer_;

    std::set<std::string> stopWords_;
    PageLibrary pageLibrary_;

    // word -> docId -> normalized TF-IDF weight。
    std::unordered_map<std::string, PostingMap> invertedIndex_;

    int abstractLength_ = 150;
};
