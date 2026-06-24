#include "../../include/online/DynamicAbstract.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace
{
std::string html_unescape(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 4, "&lt;") == 0) {
            result.push_back('<');
            i += 4;
        } else if (text.compare(i, 4, "&gt;") == 0) {
            result.push_back('>');
            i += 4;
        } else if (text.compare(i, 5, "&amp;") == 0) {
            result.push_back('&');
            i += 5;
        } else if (text.compare(i, 6, "&nbsp;") == 0) {
            result.push_back(' ');
            i += 6;
        } else {
            result.push_back(text[i++]);
        }
    }

    return result;
}

std::string strip_html_tags(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool inTag = false;
    for (char ch : text) {
        if (ch == '<') {
            inTag = true;
            result.push_back(' ');
            continue;
        }
        if (ch == '>') {
            inTag = false;
            result.push_back(' ');
            continue;
        }
        if (!inTag) {
            result.push_back(ch);
        }
    }

    return result;
}

std::string collapse_spaces(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool previousSpace = false;
    for (unsigned char ch : text) {
        bool currentSpace = std::isspace(ch) != 0;
        if (currentSpace) {
            if (!previousSpace) {
                result.push_back(' ');
            }
        } else {
            result.push_back(static_cast<char>(ch));
        }
        previousSpace = currentSpace;
    }

    return result;
}

std::string clean_content(const std::string& content)
{
    return collapse_spaces(strip_html_tags(html_unescape(content)));
}

std::vector<int> find_keyword_positions(const std::vector<std::string>& chars,
                                        const std::vector<std::string>& keywordChars)
{
    std::vector<int> positions;
    if (keywordChars.empty() || keywordChars.size() > chars.size()) {
        return positions;
    }

    for (std::size_t i = 0; i + keywordChars.size() <= chars.size(); ++i) {
        bool matched = true;
        for (std::size_t j = 0; j < keywordChars.size(); ++j) {
            if (chars[i + j] != keywordChars[j]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            positions.push_back(static_cast<int>(i));
        }
    }

    return positions;
}

double position_weight(int pos, int total)
{
    if (total <= 0) {
        return 1.0;
    }

    double ratio = static_cast<double>(pos) / total;
    if (ratio <= 0.20) {
        return 1.30;
    }
    if (ratio >= 0.80) {
        return 1.15;
    }
    return 1.00;
}

std::string join_chars(const std::vector<std::string>& chars, int begin, int end)
{
    std::string result;
    for (int i = begin; i < end; ++i) {
        result += chars[i];
    }
    return result;
}

std::string highlight_keywords(std::string text, const std::vector<std::string>& keywords)
{
    // 长词先高亮，避免较短关键词先插入 <em> 后影响较长关键词匹配。
    std::vector<std::string> sorted = keywords;
    std::sort(sorted.begin(), sorted.end(), [](const std::string& lhs, const std::string& rhs) {
        return lhs.size() > rhs.size();
    });

    std::set<std::string> handled;
    for (const auto& keyword : sorted) {
        if (keyword.empty() || handled.count(keyword) != 0) {
            continue;
        }
        handled.insert(keyword);

        std::size_t pos = 0;
        while ((pos = text.find(keyword, pos)) != std::string::npos) {
            text.replace(pos, keyword.size(), "<em>" + keyword + "</em>");
            pos += keyword.size() + 9;
        }
    }

    return text;
}
}

std::string DynamicAbstract::generate(const std::string& content,
                                      const std::vector<std::string>& keywords,
                                      int abstractLength)
{
    abstractLength = abstractLength <= 0 ? 150 : abstractLength;

    std::string plainText = clean_content(content);
    std::vector<std::string> chars = TextUtils::split_utf8_characters(plainText);
    if (chars.empty()) {
        return "";
    }

    std::vector<std::vector<int>> keywordPositions;
    std::vector<std::vector<std::string>> keywordChars;
    for (const auto& keyword : keywords) {
        keywordChars.push_back(TextUtils::split_utf8_characters(keyword));
        keywordPositions.push_back(find_keyword_positions(chars, keywordChars.back()));
    }

    struct Window {
        int begin = 0;
        int end = 0;
        double score = 0.0;
    };

    Window best;
    best.end = std::min(abstractLength, static_cast<int>(chars.size()));

    // 以每个关键词出现位置为中心生成候选窗口，然后按命中数、覆盖数和位置权重
    // 打分。逻辑保持直接，便于从代码中对应开发文档里的摘要规则。
    for (std::size_t k = 0; k < keywordPositions.size(); ++k) {
        for (int pos : keywordPositions[k]) {
            int begin = std::max(0, pos - 50);
            int end = std::min(static_cast<int>(chars.size()), begin + abstractLength);
            begin = std::max(0, end - abstractLength);

            int totalHits = 0;
            int coveredKeywords = 0;
            int firstHit = static_cast<int>(chars.size());
            int lastHit = 0;

            for (std::size_t i = 0; i < keywordPositions.size(); ++i) {
                int hitsForKeyword = 0;
                for (int hitPos : keywordPositions[i]) {
                    if (hitPos >= begin && hitPos < end) {
                        ++hitsForKeyword;
                        firstHit = std::min(firstHit, hitPos);
                        lastHit = std::max(lastHit, hitPos);
                    }
                }

                if (hitsForKeyword > 0) {
                    ++coveredKeywords;
                    totalHits += 10 + std::min(6, (hitsForKeyword - 1) * 2);
                }
            }

            double coverageBonus = keywords.empty()
                ? 0.0
                : static_cast<double>(coveredKeywords) / keywords.size() * 10.0;
            double closeBonus = (coveredKeywords >= 2 && lastHit - firstHit <= 40) ? 5.0 : 0.0;
            double score = totalHits * position_weight(pos, static_cast<int>(chars.size()))
                         + coverageBonus
                         + closeBonus;

            if (score > best.score) {
                best = {begin, end, score};
            }
        }
    }

    std::string abstract = join_chars(chars, best.begin, best.end);
    return highlight_keywords(abstract, keywords);
}
