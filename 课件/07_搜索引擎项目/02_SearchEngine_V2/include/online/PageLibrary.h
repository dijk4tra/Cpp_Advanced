#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * @brief 网页库中的一篇文档。
 */
struct Document
{
    int id = 0;
    std::string title;
    std::string link;
    std::string content;
};

/**
 * @brief 网页库偏移信息。
 */
struct PageOffset
{
    int docId = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/**
 * @brief 加载一期生成的网页库和偏移库。
 *
 * 当前课程语料规模不大，因此启动时直接把网页解析到内存，查询时不再反复读
 * 磁盘。偏移库仍用于从 pages.dat 中准确切出每一篇 <doc> 片段。
 */
class PageLibrary
{
public:
    /**
     * @brief 加载 offsets.dat 和 pages.dat。
     * @param pagesFile 网页库路径。
     * @param offsetsFile 偏移库路径。
     * @throws std::runtime_error 文件无法打开或网页片段无法读取时抛出。
     */
    void load(const std::string& pagesFile, const std::string& offsetsFile);

    /**
     * @brief 根据文档 id 查询文档。
     * @return 找到时返回 Document 指针，否则返回 nullptr。
     */
    const Document* find(int docId) const;

    /**
     * @brief 返回已加载文档数量。
     */
    std::size_t size() const { return documents_.size(); }

private:
    void load_offsets(const std::string& offsetsFile);
    Document parse_document(const std::string& xmlText) const;

private:
    std::unordered_map<int, PageOffset> offsets_;
    std::unordered_map<int, Document> documents_;
};
