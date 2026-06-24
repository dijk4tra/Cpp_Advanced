#pragma once

#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 词典中的一条记录。
 *
 * 一期词典文件只保存 word 和 frequency；行号由文件物理顺序隐含表示。二期
 * 加载时保留相同约定，使字符索引中的 lineNo 可以直接定位到词典记录。
 */
struct DictEntry
{
    std::string word;
    int frequency = 0;
};

/**
 * @brief 在线关键字推荐模块。
 *
 * 该模块启动时加载一期生成的中英文词典和字符索引。查询时先根据输入字符从
 * 索引中召回候选词，再使用 UTF-8 感知的编辑距离、词频和字典序排序。
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
     */
    std::string recommend_json(const std::string& query,
                               const std::string& lang,
                               int topK) const;

private:
    using CharIndex = std::unordered_map<std::string, std::vector<int>>;

    void load_dict(const std::string& filename, std::vector<DictEntry>& dict);
    void load_index(const std::string& filename, CharIndex& index);

    static std::vector<std::string> split_query(const std::string& query,
                                                const std::string& lang);
    static int edit_distance(const std::string& lhs,
                             const std::string& rhs,
                             const std::string& lang);

private:
    // dict[0] 故意保留为空记录，使一期索引中从 1 开始的 lineNo 可以直接使用。
    std::vector<DictEntry> cnDict_;
    std::vector<DictEntry> enDict_;

    // character -> dictionary line numbers。
    CharIndex cnIndex_;
    CharIndex enIndex_;
};
