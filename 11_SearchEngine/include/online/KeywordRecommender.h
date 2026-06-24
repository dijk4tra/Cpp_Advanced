#pragma once

#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 词典中的一条记录。
 *
 * 一期词典文件只保存 word 和 frequency；行号由文件物理顺序隐含表示。
 * 二期加载时保留相同约定，使字符索引中的 lineNo 可以直接定位到词典记录。
 */
struct DictEntry
{
    // 候选推荐词本身。
    std::string word;

    // 该词在语料中出现的频次，编辑距离相同时按频次降序排序。
    int frequency = 0;
};

/**
 * @brief 在线关键字推荐模块。
 *
 * 该模块启动时加载一期生成的中英文词典和字符索引。
 * 查询时先根据输入字符从索引中召回候选词，再使用 UTF-8 感知的编辑距离、词频和字典序排序。
 *
 * 对外只返回 JSON 字符串，便于 TLV 服务和 HTTP 服务复用同一个业务模块。
 * 加载完成后成员数据只读，第二期不在这里维护缓存或会话状态。
 */
class KeywordRecommender
{
public:
    /**
     * @brief 加载中英文词典和索引。
     *
     * @param cnDict 中文词典路径。
     * @param cnIndex 中文字符索引路径。
     * @param enDict 英文词典路径。
     * @param enIndex 英文字符索引路径。
     * @throws std::runtime_error 任一文件无法打开时抛出。
     */
    void load(const std::string& cnDict,
              const std::string& cnIndex,
              const std::string& enDict,
              const std::string& enIndex);

    /**
     * @brief 根据用户输入生成推荐词 JSON。
     *
     * @param query 用户输入的关键词。
     * @param lang 语言，支持 "cn"、"en" 和空字符串自动判断。
     * @param topK 返回数量。
     * @return JSON 字符串。
     * @throws utf8::exception 中文查询或词典项编码非法时可能抛出。
     */
    std::string recommend_json(const std::string& query,
                               const std::string& lang,
                               int topK) const;

private:
    // 单字/字符 -> 包含该字符的词典行号集合。
    // 中文 key 为一个 UTF-8 汉字，英文 key 为一个小写字母。
    using CharIndex = std::unordered_map<std::string, std::vector<int>>;

    /**
     * @brief 加载 `word frequency` 格式的词典文件。
     * @param filename 词典文件路径。
     * @param dict 输出参数，加载前会被清空并插入一个空的 0 号记录。
     * @throws std::runtime_error 文件无法打开时抛出。
     */
    void load_dict(const std::string& filename, std::vector<DictEntry>& dict);

    /**
     * @brief 加载字符索引文件。
     * @param filename 索引文件路径，每行格式为 `character lineNo...`。
     * @param index 输出参数，加载前会被清空。
     * @throws std::runtime_error 文件无法打开时抛出。
     */
    void load_index(const std::string& filename, CharIndex& index);

    /**
     * @brief 将用户输入拆成用于索引召回的去重字符集合。
     * @param query 原始查询词。
     * @param lang 已归一化的语言类型，取值为 "cn" 或 "en"。
     * @return 去重后的字符列表。
     * @throws utf8::exception 中文查询编码非法时可能抛出。
     */
    static std::vector<std::string> split_query(const std::string& query,
                                                const std::string& lang);

    /**
     * @brief 计算两个词之间的编辑距离。
     *
     * 英文按单字母计算，中文按 UTF-8 字符计算，避免把一个汉字拆成多个字节。
     *
     * @param lhs 用户输入。
     * @param rhs 候选词。
     * @param lang 已归一化的语言类型。
     * @return 插入、删除、替换的最少操作次数。
     * @throws utf8::exception 中文词编码非法时可能抛出。
     */
    static int edit_distance(const std::string& lhs,
                             const std::string& rhs,
                             const std::string& lang);

private:
    // dict[0] 故意保留为空记录，使一期索引中从 1 开始的 lineNo 可以直接使用。
    std::vector<DictEntry> cnDict_;
    std::vector<DictEntry> enDict_;

    // character -> dictionary line numbers。行号来自一期离线索引文件。
    CharIndex cnIndex_;
    CharIndex enIndex_;
};
