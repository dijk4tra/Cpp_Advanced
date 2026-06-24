#include "../../include/online/KeywordRecommender.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
// 本文件中的辅助函数只服务 KeywordRecommender，不属于类的公共接口，
// 因此放在匿名命名空间中限制可见范围。

/**
 * @brief 判断用户输入更像中文还是英文。
 *
 * 只要包含一个非 ASCII 字节，就按中文处理。课程数据主要区分中文语料和英文
 * 语料，这个简单规则足够直观，也避免引入额外语言识别逻辑。
 *
 * @param query 用户输入。
 * @param lang 请求中显式传入的语言类型。
 * @return "cn" 或 "en"。
 */
std::string normalize_lang(const std::string& query, const std::string& lang)
{
    // 如果客户端已经明确指定 "cn" 或 "en"，优先尊重客户端选择。
    if (lang == "cn" || lang == "en") {
        return lang;
    }

    // UTF-8 中文字符一定包含高位字节，因此看到非 ASCII 字节即可按中文处理。
    // unsigned char 可以避免 char 被当作负数后再比较时产生误判。
    for (unsigned char ch : query) {
        if (ch >= 0x80) {
            return "cn";
        }
    }
    return "en";
}

/**
 * @brief 将英文查询拆成去重后的小写字母集合。
 *
 * 英文推荐的一期索引是按字符建立的，因此在线召回也只需要查询中出现过的字母。
 *
 * @param query 用户原始输入。
 * @return 去重且按字母序排列的字符列表。
 */
std::vector<std::string> split_english_characters(const std::string& query)
{
    std::string normalized = TextUtils::normalize_english_line(query);
    std::vector<std::string> characters;
    std::set<char> uniqueChars;

    // normalize_english_line 已经把非字母转为空格，并把大写字母转成小写。
    // set 插入重复字符时会自动忽略，所以查询 "apple" 只会保留 a、e、l、p。
    for (char ch : normalized) {
        if (ch >= 'a' && ch <= 'z') {
            uniqueChars.insert(ch);
        }
    }

    // set 让字符天然去重且有序，结果稳定，便于测试和复现。
    for (char ch : uniqueChars) {
        // emplace_back(1, ch) 会在 vector 尾部直接构造一个长度为 1 的 string。
        // 这样比先创建临时 string 再 push_back 更直接。
        characters.emplace_back(1, ch);
    }
    return characters;
}

/**
 * @brief 将一个词拆成编辑距离算法使用的基本单位。
 *
 * 英文按单字母计算；中文按完整 UTF-8 字符计算，不能按字节拆分。
 *
 * @param word 待拆分词语。
 * @param lang 已归一化语言类型。
 * @return 编辑距离 DP 使用的 token 序列。
 */
std::vector<std::string> split_word(const std::string& word, const std::string& lang)
{
    if (lang == "en") {
        std::vector<std::string> result;
        // 英文一个字节就是一个字母，最多产生 word.size() 个元素。
        result.reserve(word.size());
        for (char ch : word) {
            result.emplace_back(1, ch);
        }
        return result;
    }

    return TextUtils::split_utf8_characters(word);
}
}

/**
 * @brief 加载关键字推荐所需的全部离线数据。
 * @throws std::runtime_error 任一词典或索引文件无法打开时抛出。
 */
void KeywordRecommender::load(const std::string& cnDict,
                              const std::string& cnIndex,
                              const std::string& enDict,
                              const std::string& enIndex)
{
    // 加载顺序没有业务依赖，但保持“中文词典、中文索引、英文词典、英文索引”
    // 与配置文件顺序一致，启动报错时更容易定位。
    // 这些数据加载完后在线阶段只读，因此多个 muduo 线程可共享同一个对象。
    load_dict(cnDict, cnDict_);
    load_index(cnIndex, cnIndex_);
    load_dict(enDict, enDict_);
    load_index(enIndex, enIndex_);
}

/**
 * @brief 根据查询词生成推荐结果 JSON。
 * @throws utf8::exception 中文输入或词典项编码非法时可能抛出。
 */
std::string KeywordRecommender::recommend_json(const std::string& query,
                                               const std::string& lang,
                                               int topK) const
{
    // lang 允许为空，由 normalize_lang 根据查询内容自动推断。
    std::string realLang = normalize_lang(query, lang);
    // topK 小于等于 0 没有实际意义，直接回退到默认返回 5 条。
    topK = topK <= 0 ? 5 : topK;

    nlohmann::json response;
    // nlohmann::json 可以像 map 一样用 [] 设置字段。
    // results 明确设置为空数组，保证没有结果时前端仍能按数组处理。
    response["type"] = "keyword";
    response["query"] = query;
    response["lang"] = realLang;
    response["results"] = nlohmann::json::array();

    std::vector<std::string> characters = split_query(query, realLang);
    if (characters.empty()) {
        // 没有可用于召回的字符时返回空 results，而不是报错，前端更容易处理。
        return response.dump();
    }

    // 根据语言选择对应的词典和字符索引。const auto& 表示引用已有容器，
    // 不复制大词典数据。
    const auto& dict = realLang == "cn" ? cnDict_ : enDict_;
    const auto& index = realLang == "cn" ? cnIndex_ : enIndex_;

    // 通过字符索引召回候选词行号。使用 set 完成去重，并让遍历顺序稳定。
    std::set<int> candidateLines;
    for (const auto& ch : characters) {
        auto it = index.find(ch);
        if (it == index.end()) {
            continue;
        }
        // insert(first, last) 可以一次性把 vector 中的所有行号插入 set。
        // set 会自动去重，避免同一个候选词因为多个字符命中而重复计算。
        candidateLines.insert(it->second.begin(), it->second.end());
    }

    // 候选结构体只在本函数中使用，用于排序时同时保存词、词频和编辑距离。
    struct Candidate {
        std::string word;
        int frequency = 0;
        int distance = 0;
    };

    std::vector<Candidate> candidates;
    for (int lineNo : candidateLines) {
        // 词典第 0 项是占位记录，有效 lineNo 从 1 开始；索引文件如果出现异常
        // 行号，这里直接跳过。
        if (lineNo <= 0 || lineNo >= static_cast<int>(dict.size())) {
            continue;
        }

        const DictEntry& entry = dict[lineNo];
        // push_back({ ... }) 使用列表初始化构造 Candidate。
        candidates.push_back({entry.word,
                              entry.frequency,
                              edit_distance(query, entry.word, realLang)});
    }

    // 排序规则与推荐算法一致：
    // 1. 编辑距离越小越相近；
    // 2. 距离相同，词频越高越常用；
    // 3. 前两者相同，用字典序保证结果稳定。
    // lambda 返回 true 表示 lhs 应排在 rhs 前面。
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        if (lhs.frequency != rhs.frequency) {
            return lhs.frequency > rhs.frequency;
        }
        return lhs.word < rhs.word;
    });

    // static_cast<int> 明确把 size_t 转成 int，避免 signed/unsigned 比较警告。
    int count = std::min(topK, static_cast<int>(candidates.size()));
    for (int i = 0; i < count; ++i) {
        // push_back 直接追加一个 JSON object 到 results 数组。
        response["results"].push_back({
            {"word", candidates[i].word},
            {"distance", candidates[i].distance},
            {"frequency", candidates[i].frequency}
        });
    }

    return response.dump();
}

/**
 * @brief 加载一期生成的词典文件。
 * @param filename 词典路径。
 * @param dict 输出词典。
 * @throws std::runtime_error 文件无法打开时抛出。
 */
void KeywordRecommender::load_dict(const std::string& filename, std::vector<DictEntry>& dict)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open dictionary: " + filename);
    }

    dict.clear();
    dict.push_back(DictEntry{}); // 让 lineNo 从 1 开始直接作为下标。

    std::string word;
    int frequency = 0;
    // 词典文件每行是 `word frequency`。operator>> 自动按空白分隔读取。
    // 当读到文件末尾或遇到格式错误时，流状态变为 false，循环结束。
    while (ifs >> word >> frequency) {
        dict.push_back({word, frequency});
    }
}

/**
 * @brief 加载字符到词典行号的索引。
 * @param filename 索引文件路径。
 * @param index 输出索引。
 * @throws std::runtime_error 文件无法打开时抛出。
 */
void KeywordRecommender::load_index(const std::string& filename, CharIndex& index)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open dictionary index: " + filename);
    }

    index.clear();
    std::string line;
    // getline 每次读取一整行，适合处理“一个字符 + 多个行号”的变长行。
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string character;
        // 每行第一个字段是字符，后续字段是包含该字符的词典行号。
        iss >> character;
        if (character.empty()) {
            continue;
        }

        int lineNo = 0;
        while (iss >> lineNo) {
            // operator[] 在 key 不存在时会自动创建一个空 vector，再追加 lineNo。
            index[character].push_back(lineNo);
        }
    }
}

/**
 * @brief 拆分查询词，得到用于字符索引召回的去重字符。
 */
std::vector<std::string> KeywordRecommender::split_query(const std::string& query,
                                                         const std::string& lang)
{
    if (lang == "en") {
        return split_english_characters(query);
    }

    std::vector<std::string> result;
    std::set<std::string> uniqueChars;
    // 中文只保留真正的汉字字符，标点、数字、英文字母不参与中文词典召回。
    for (const auto& ch : TextUtils::split_utf8_characters(query)) {
        if (TextUtils::is_chinese_word(ch)) {
            uniqueChars.insert(ch);
        }
    }

    // assign(first, last) 用迭代器区间替换 result 的内容。
    // set 已经按字典序去重，因此 result 也稳定有序。
    result.assign(uniqueChars.begin(), uniqueChars.end());
    return result;
}

/**
 * @brief 使用动态规划计算编辑距离。
 *
 * dp[i][j] 表示 lhs 前 i 个单位变成 rhs 前 j 个单位所需的最少操作数。
 */
int KeywordRecommender::edit_distance(const std::string& lhs,
                                      const std::string& rhs,
                                      const std::string& lang)
{
    std::vector<std::string> left = split_word(lhs, lang);
    std::vector<std::string> right = split_word(rhs, lang);

    // 多开一行一列用于表示空串。dp[0][j] 是空串插入 j 个字符的代价；
    // dp[i][0] 是删除 i 个字符的代价。
    // vector(size, value) 会创建指定数量的元素，并把每个元素初始化为 value。
    std::vector<std::vector<int>> dp(left.size() + 1,
                                     std::vector<int>(right.size() + 1, 0));

    // 初始化第一列：lhs 前 i 个字符变为空串，只能删除 i 次。
    for (std::size_t i = 0; i <= left.size(); ++i) {
        dp[i][0] = static_cast<int>(i);
    }
    // 初始化第一行：空串变成 rhs 前 j 个字符，只能插入 j 次。
    for (std::size_t j = 0; j <= right.size(); ++j) {
        dp[0][j] = static_cast<int>(j);
    }

    // 从短串逐步扩展到完整字符串。每个状态只依赖左边、上边和左上角三个状态。
    for (std::size_t i = 1; i <= left.size(); ++i) {
        for (std::size_t j = 1; j <= right.size(); ++j) {
            if (left[i - 1] == right[j - 1]) {
                // 当前字符相同，不需要额外操作。
                dp[i][j] = dp[i - 1][j - 1];
            } else {
                // 三种选择分别对应删除、插入、替换，取最小值。
                dp[i][j] = std::min({dp[i - 1][j] + 1,
                                     dp[i][j - 1] + 1,
                                     dp[i - 1][j - 1] + 1});
            }
        }
    }

    return dp[left.size()][right.size()];
}
