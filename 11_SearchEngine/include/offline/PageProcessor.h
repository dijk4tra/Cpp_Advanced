#pragma once

#include <cppjieba/Jieba.hpp>
#include <map>
#include <set>
#include <simhash/Simhasher.hpp>
#include <string>
#include <vector>

/**
 * @brief 构建网页搜索所需的网页库、偏移库和倒排索引库。
 *
 * 完整流程为：从 XML 的 `<item>` 提取文档，使用 64 位 SimHash 去重并重新
 * 连续编号，生成 pages.dat 和 offsets.dat，最后按公式计算归一化 TF-IDF
 * 并生成 invert_index.dat。
 *
 * 三个输出文件共享同一组去重后的文档 id。调用 process() 时必须保持阶段顺序，
 * 不能单独跳过文档提取或去重。该类持有可变中间状态，不是线程安全的。
 */
class PageProcessor
{
public:
    /**
     * @brief 初始化中文分词器、SimHash 计算器并加载停用词。
     * @param stopWordsFile 网页正文分词使用的中文停用词文件路径。
     * @throws std::runtime_error 停用词文件无法打开时抛出。
     * @throws cppjieba 或 simhash 初始化其词典失败时产生的异常会自然传播。
     */
    explicit PageProcessor(const std::string& stopWordsFile);

    /**
     * @brief 执行网页搜索离线建库的全部阶段。
     *
     * @param dir 网页 XML 语料目录。
     * @param pages 网页库输出路径，内容为连续的 `<doc>` 记录。
     * @param offsets 偏移库输出路径，格式为 `docId offset length`。
     * @param invertIndex 倒排索引输出路径，格式为
     *        `word docId weight [docId weight]...`。
     * @throws std::runtime_error 输入目录、停用词或任一输出文件不可用时抛出。
     * @throws utf8::exception 网页分词结果包含非法 UTF-8 序列时可能抛出。
     */
    void process(const std::string& dir,
                 const std::string& pages,
                 const std::string& offsets,
                 const std::string& invertIndex);

private:
    /**
     * @brief 保存一篇可进入网页库的内存文档。
     *
     * id 在 XML 提取后暂时连续编号，在 SimHash 去重后会再次从 1 连续编号，
     * 以保证网页库、偏移库和倒排索引引用同一编号。
     */
    struct Document {
        // 文档唯一编号；去重后从 1 开始连续递增。
        int id = 0;

        // 原网页地址；XML 中缺少 <link> 时为空字符串。
        std::string link;

        // 网页标题；XML 中缺少 <title> 时为空字符串。
        std::string title;

        // 可检索正文；优先取 <content>，否则取 <description>。
        std::string content;
    };

private:
    /**
     * @brief 扫描并解析 XML 语料，将有效 `<item>` 转换为 Document。
     * @param dir 网页 XML 文件目录。
     * @throws std::runtime_error dir 无法扫描时抛出。单个 XML 解析失败只记录日志
     *         并跳过，不抛异常。
     */
    void extract_documents(const std::string& dir);

    /**
     * @brief 使用 SimHash 删除汉明距离不超过 3 的近似重复文档。
     *
     * 当前朴素实现逐一比较指纹，时间复杂度为 O(n^2)，去重后重新连续编号。
     * @throws simhash 或其分词依赖产生的异常会自然传播。
     */
    void deduplicate_documents();

    /**
     * @brief 为去重文档生成网页库和按字节计数的偏移库。
     * @param pages 网页库输出路径。
     * @param offsets 偏移库输出路径。
     * @throws std::runtime_error 任一输出文件无法打开时抛出。
     */
    void build_pages_and_offsets(const std::string& pages,
                                 const std::string& offsets);

    /**
     * @brief 对去重文档计算归一化 TF-IDF 并生成倒排索引。
     * @param filename 倒排索引输出路径。
     * @throws std::runtime_error 输出文件无法打开时抛出。
     * @throws utf8::exception 分词结果包含非法 UTF-8 序列时可能抛出。
     */
    void build_inverted_index(const std::string& filename);

private:
    // 中文分词器，供网页关键词统计使用。
    cppjieba::Jieba tokenizer_;

    // SimHash 计算器，供近似重复文档检测使用。
    simhash::Simhasher hasher_;

    // 网页正文停用词集合。
    std::set<std::string> stopWords_;

    // 当前一次 process() 中提取并最终去重后的文档集合。
    std::vector<Document> documents_;

    // word -> (docId -> normalized TF-IDF weight)。内层映射即 posting list。
    std::map<std::string, std::map<int, double>> invertedIndex_;
};
