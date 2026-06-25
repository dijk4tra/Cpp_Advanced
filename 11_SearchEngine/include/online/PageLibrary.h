#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * @brief 网页库中的一篇文档。
 *
 * 结构体字段对应 pages.dat 中一条 `<doc>` 记录的主要子标签。在线查询最终
 * 返回结果时，需要从这里取得标题、链接和正文，再生成动态摘要。
 */
struct Document
{
    // 文档编号，与 offsets.dat 和 invert_index.dat 中使用的 docId 一致
    int id = 0;

    // 网页标题；离线语料缺少标题时可能为空
    std::string title;

    // 网页原始链接；离线语料缺少链接时可能为空
    std::string link;

    // 网页正文，用于生成动态摘要
    std::string content;
};

/**
 * @brief 网页库偏移信息。
 *
 * offsets.dat 使用 `docId offset length` 三列保存每篇文档在 pages.dat 中的
 * 字节位置。在线加载时依靠该结构从大文件中切出单篇 XML 片段。
 */
struct PageOffset
{
    // 文档编号
    int docId = 0;

    // 文档片段在 pages.dat 中的起始字节偏移
    std::uint64_t offset = 0;

    // 文档片段的字节长度
    std::uint64_t length = 0;
};

/**
 * @brief 加载一期生成的网页库和偏移库。
 *
 * 当前语料规模不大，因此启动时直接把网页解析到内存，查询时不再反复读磁盘。
 * 偏移库仍用于从 pages.dat 中准确切出每一篇 <doc> 片段。
 *
 * load() 完成后，find() 只做哈希表查询，不修改内部状态，可以被搜索线程并发读取。
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
    /**
     * @brief 加载 offsets.dat 到内存。
     * @param offsetsFile 偏移库路径。
     * @throws std::runtime_error 文件无法打开时抛出。
     */
    void load_offsets(const std::string& offsetsFile);

    /**
     * @brief 将一段 `<doc>...</doc>` XML 文本解析为 Document。
     * @param xmlText 从 pages.dat 中切出的单篇文档 XML。
     * @return 解析成功时返回有效 Document；失败时返回 id 为 0 的空文档。
     */
    Document parse_document(const std::string& xmlText) const;

private:
    // docId -> 在 pages.dat 中的位置和长度。
    std::unordered_map<int, PageOffset> offsets_;

    // docId -> 已解析好的文档内容。查询阶段直接读该表。
    std::unordered_map<int, Document> documents_;
};
