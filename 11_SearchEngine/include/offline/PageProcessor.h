#pragma once

#include <cppjieba/Jieba.hpp>
#include <map>
#include <set>
#include <simhash/Simhasher.hpp>
#include <string>
#include <vector>

// PageProcessor 负责“网页搜索”的离线建库：
// 1. 从 XML 语料中提取网页文档。
// 2. 使用 SimHash 对相似网页去重。
// 3. 生成网页库 pages.dat 和网页偏移库 offsets.dat。
// 4. 使用 TF-IDF 生成倒排索引 invert_index.dat。
class PageProcessor
{
public:
    explicit PageProcessor(const std::string& stopWordsFile);

    void process(const std::string& dir,
                 const std::string& pages,
                 const std::string& offsets,
                 const std::string& invertIndex);

private:
    struct Document {
        int id = 0;
        std::string link;
        std::string title;
        std::string content;
    };

private:
    void extract_documents(const std::string& dir);
    void deduplicate_documents();
    void build_pages_and_offsets(const std::string& pages,
                                 const std::string& offsets);
    void build_inverted_index(const std::string& filename);

private:
    cppjieba::Jieba tokenizer_;
    simhash::Simhasher hasher_;
    std::set<std::string> stopWords_;
    std::vector<Document> documents_;
    std::map<std::string, std::map<int, double>> invertedIndex_;
};
