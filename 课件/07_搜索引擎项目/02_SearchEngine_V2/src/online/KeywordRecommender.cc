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
/**
 * @brief 判断用户输入更像中文还是英文。
 *
 * 只要包含一个非 ASCII 字节，就按中文处理。课程数据主要区分中文语料和英文
 * 语料，这个简单规则足够直观，也避免引入额外语言识别逻辑。
 */
std::string normalize_lang(const std::string& query, const std::string& lang)
{
    if (lang == "cn" || lang == "en") {
        return lang;
    }

    for (unsigned char ch : query) {
        if (ch >= 0x80) {
            return "cn";
        }
    }
    return "en";
}

std::vector<std::string> split_english_characters(const std::string& query)
{
    std::string normalized = TextUtils::normalize_english_line(query);
    std::vector<std::string> characters;
    std::set<char> uniqueChars;

    for (char ch : normalized) {
        if (ch >= 'a' && ch <= 'z') {
            uniqueChars.insert(ch);
        }
    }

    for (char ch : uniqueChars) {
        characters.emplace_back(1, ch);
    }
    return characters;
}

std::vector<std::string> split_word(const std::string& word, const std::string& lang)
{
    if (lang == "en") {
        std::vector<std::string> result;
        result.reserve(word.size());
        for (char ch : word) {
            result.emplace_back(1, ch);
        }
        return result;
    }

    return TextUtils::split_utf8_characters(word);
}
}

void KeywordRecommender::load(const std::string& cnDict,
                              const std::string& cnIndex,
                              const std::string& enDict,
                              const std::string& enIndex)
{
    load_dict(cnDict, cnDict_);
    load_index(cnIndex, cnIndex_);
    load_dict(enDict, enDict_);
    load_index(enIndex, enIndex_);
}

std::string KeywordRecommender::recommend_json(const std::string& query,
                                               const std::string& lang,
                                               int topK) const
{
    std::string realLang = normalize_lang(query, lang);
    topK = topK <= 0 ? 5 : topK;

    nlohmann::json response;
    response["type"] = "keyword";
    response["query"] = query;
    response["lang"] = realLang;
    response["results"] = nlohmann::json::array();

    std::vector<std::string> characters = split_query(query, realLang);
    if (characters.empty()) {
        return response.dump();
    }

    const auto& dict = realLang == "cn" ? cnDict_ : enDict_;
    const auto& index = realLang == "cn" ? cnIndex_ : enIndex_;

    // 通过字符索引召回候选词行号。使用 set 完成去重，并让遍历顺序稳定。
    std::set<int> candidateLines;
    for (const auto& ch : characters) {
        auto it = index.find(ch);
        if (it == index.end()) {
            continue;
        }
        candidateLines.insert(it->second.begin(), it->second.end());
    }

    struct Candidate {
        std::string word;
        int frequency = 0;
        int distance = 0;
    };

    std::vector<Candidate> candidates;
    for (int lineNo : candidateLines) {
        if (lineNo <= 0 || lineNo >= static_cast<int>(dict.size())) {
            continue;
        }

        const DictEntry& entry = dict[lineNo];
        candidates.push_back({entry.word,
                              entry.frequency,
                              edit_distance(query, entry.word, realLang)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        if (lhs.frequency != rhs.frequency) {
            return lhs.frequency > rhs.frequency;
        }
        return lhs.word < rhs.word;
    });

    int count = std::min(topK, static_cast<int>(candidates.size()));
    for (int i = 0; i < count; ++i) {
        response["results"].push_back({
            {"word", candidates[i].word},
            {"distance", candidates[i].distance},
            {"frequency", candidates[i].frequency}
        });
    }

    return response.dump();
}

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
    while (ifs >> word >> frequency) {
        dict.push_back({word, frequency});
    }
}

void KeywordRecommender::load_index(const std::string& filename, CharIndex& index)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open dictionary index: " + filename);
    }

    index.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string character;
        iss >> character;
        if (character.empty()) {
            continue;
        }

        int lineNo = 0;
        while (iss >> lineNo) {
            index[character].push_back(lineNo);
        }
    }
}

std::vector<std::string> KeywordRecommender::split_query(const std::string& query,
                                                         const std::string& lang)
{
    if (lang == "en") {
        return split_english_characters(query);
    }

    std::vector<std::string> result;
    std::set<std::string> uniqueChars;
    for (const auto& ch : TextUtils::split_utf8_characters(query)) {
        if (TextUtils::is_chinese_word(ch)) {
            uniqueChars.insert(ch);
        }
    }

    result.assign(uniqueChars.begin(), uniqueChars.end());
    return result;
}

int KeywordRecommender::edit_distance(const std::string& lhs,
                                      const std::string& rhs,
                                      const std::string& lang)
{
    std::vector<std::string> left = split_word(lhs, lang);
    std::vector<std::string> right = split_word(rhs, lang);

    std::vector<std::vector<int>> dp(left.size() + 1,
                                     std::vector<int>(right.size() + 1, 0));

    for (std::size_t i = 0; i <= left.size(); ++i) {
        dp[i][0] = static_cast<int>(i);
    }
    for (std::size_t j = 0; j <= right.size(); ++j) {
        dp[0][j] = static_cast<int>(j);
    }

    for (std::size_t i = 1; i <= left.size(); ++i) {
        for (std::size_t j = 1; j <= right.size(); ++j) {
            if (left[i - 1] == right[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1];
            } else {
                dp[i][j] = std::min({dp[i - 1][j] + 1,
                                     dp[i][j - 1] + 1,
                                     dp[i - 1][j - 1] + 1});
            }
        }
    }

    return dp[left.size()][right.size()];
}
