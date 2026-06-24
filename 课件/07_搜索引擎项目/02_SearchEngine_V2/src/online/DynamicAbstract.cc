#include "../../include/online/DynamicAbstract.h"

#include "../../include/common/TextUtils.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace
{
// 匿名命名空间中的函数只在当前 .cc 文件可见。
// 这些函数都是 DynamicAbstract 的内部辅助逻辑，不需要暴露给其他模块调用。

/**
 * @brief 将常见 HTML 实体还原为普通字符。
 *
 * 离线网页库为了保存为 XML，会把 `<`、`>`、`&` 等特殊字符转义。生成摘要前
 * 先做反转义，可以让摘要展示更接近原始正文。
 *
 * @param text 原始文本。
 * @return 完成实体还原的新字符串。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
std::string html_unescape(const std::string& text)
{
    std::string result;
    // 反转义只会缩短或保持长度，预留原长度可减少扩容次数。
    // reserve() 只改变容量，不改变字符串长度，因此后面仍然需要 push_back 逐个写入。
    result.reserve(text.size());

    // compare(pos, count, str) 可在不创建子串的情况下判断当前位置是否匹配。
    // 每命中一个实体，就写入对应字符并跳过实体长度；否则复制当前字节。
    // 这里使用 size_t 作为下标类型，是因为 string::size() 返回值就是 size_t。
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
            // text[i++] 先取当前字节，再把 i 向后移动 1 个字节。
            // 这里处理的是 ASCII 实体和普通字节，不会主动拆分中文字符。
            result.push_back(text[i++]);
        }
    }

    return result;
}

/**
 * @brief 移除正文中的 HTML 标签。
 *
 * 当前网页库中的 content 可能残留少量标签。摘要只需要可读文本，因此遇到
 * `<...>` 时跳过标签内部字符，并用空格隔开标签前后的文本，避免词语粘连。
 *
 * @param text 已完成实体还原的文本。
 * @return 去除标签后的文本。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
std::string strip_html_tags(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    // inTag 表示当前位置是否处在一段 HTML 标签内部。
    bool inTag = false;
    // 范围 for 会按顺序读取 text 中的每个字节。这里不需要修改 text，所以按值读取 char。
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
            // 只有不在标签内部的字符才会进入最终摘要文本。
            result.push_back(ch);
        }
    }

    return result;
}

/**
 * @brief 把连续空白压缩成一个普通空格。
 *
 * 去标签后正文中可能出现换行、制表符和多个连续空格。压缩空白可以让摘要
 * 片段更紧凑，也方便按字符窗口截取。
 *
 * @param text 原始文本。
 * @return 空白规整后的文本。
 * @note 异常处理：除内存分配失败外不主动抛出异常。
 */
std::string collapse_spaces(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool previousSpace = false;
    // ctype 系列函数要求参数能表示为 unsigned char，避免负 char 导致未定义行为。
    for (unsigned char ch : text) {
        // std::isspace 返回非 0 表示空白字符，这里转成 bool 便于阅读。
        bool currentSpace = std::isspace(ch) != 0;
        if (currentSpace) {
            if (!previousSpace) {
                result.push_back(' ');
            }
        } else {
            // ch 是 unsigned char，push_back 需要 char，因此显式转换回来。
            result.push_back(static_cast<char>(ch));
        }
        previousSpace = currentSpace;
    }

    return result;
}

/**
 * @brief 按摘要生成需要清洗网页正文。
 */
std::string clean_content(const std::string& content)
{
    // 顺序不能随意调整：先还原实体，才能把原本被转义的标签边界识别出来；
    // 最后再压缩清洗过程中产生的多余空白。
    return collapse_spaces(strip_html_tags(html_unescape(content)));
}

/**
 * @brief 在字符数组中查找某个关键词的所有出现位置。
 *
 * @param chars 正文按 UTF-8 字符拆分后的序列。
 * @param keywordChars 关键词按 UTF-8 字符拆分后的序列。
 * @return 关键词在 chars 中出现的起始下标列表。
 */
std::vector<int> find_keyword_positions(const std::vector<std::string>& chars,
                                        const std::vector<std::string>& keywordChars)
{
    std::vector<int> positions;
    if (keywordChars.empty() || keywordChars.size() > chars.size()) {
        return positions;
    }

    // 朴素逐位置匹配。摘要窗口较短，代码直观比引入复杂字符串算法更适合课程项目。
    for (std::size_t i = 0; i + keywordChars.size() <= chars.size(); ++i) {
        bool matched = true;
        // 内层循环逐字符比较 keywordChars 是否完全出现在 chars 的当前位置。
        for (std::size_t j = 0; j < keywordChars.size(); ++j) {
            if (chars[i + j] != keywordChars[j]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            // 外部打分逻辑使用 int 下标，统一在这里做类型转换。
            positions.push_back(static_cast<int>(i));
        }
    }

    return positions;
}

/**
 * @brief 根据关键词出现位置返回位置权重。
 *
 * @param pos 关键词在全文字符序列中的位置。
 * @param total 全文字符数。
 * @return 开头 20% 返回 1.30，结尾 20% 返回 1.15，中间返回 1.00。
 */
double position_weight(int pos, int total)
{
    if (total <= 0) {
        return 1.0;
    }

    double ratio = static_cast<double>(pos) / total;
    // static_cast<double> 防止整数除法。若不转换，pos / total 会先得到 0 或 1，
    // 位置比例就无法正确区分开头、中间和结尾。
    if (ratio <= 0.20) {
        return 1.30;
    }
    if (ratio >= 0.80) {
        return 1.15;
    }
    return 1.00;
}

/**
 * @brief 将字符区间重新拼成字符串。
 * @param chars UTF-8 字符序列。
 * @param begin 起始下标，包含。
 * @param end 结束下标，不包含。
 * @return `[begin, end)` 区间对应的字符串。
 */
std::string join_chars(const std::vector<std::string>& chars, int begin, int end)
{
    std::string result;
    // chars 中每个元素都是一个完整 UTF-8 字符，逐个拼接不会截断中文字符。
    for (int i = begin; i < end; ++i) {
        result += chars[i];
    }
    return result;
}

/**
 * @brief 在摘要中用 `<em>` 标记命中的关键词。
 *
 * @param text 原始摘要文本。
 * @param keywords 查询关键词列表。
 * @return 带高亮标签的摘要。
 */
std::string highlight_keywords(std::string text, const std::vector<std::string>& keywords)
{
    // 长词先高亮，避免较短关键词先插入 <em> 后影响较长关键词匹配。
    std::vector<std::string> sorted = keywords;
    // lambda 表达式用于告诉 sort 如何比较两个关键词。
    // const std::string& 避免在排序比较过程中频繁复制字符串。
    std::sort(sorted.begin(), sorted.end(), [](const std::string& lhs, const std::string& rhs) {
        return lhs.size() > rhs.size();
    });

    std::set<std::string> handled;
    for (const auto& keyword : sorted) {
        if (keyword.empty() || handled.count(keyword) != 0) {
            continue;
        }
        // handled 防止同一个关键词重复处理，避免嵌套插入 <em>。
        handled.insert(keyword);

        std::size_t pos = 0;
        // find 从 pos 位置开始查找 keyword。找不到时返回 string::npos。
        while ((pos = text.find(keyword, pos)) != std::string::npos) {
            // replace 会把当前位置的 keyword 替换成带 <em> 的新文本。
            text.replace(pos, keyword.size(), "<em>" + keyword + "</em>");
            // "<em>" 与 "</em>" 一共 9 个字节，跳过刚插入的高亮片段。
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
    // 调用方传入非法长度时使用默认值，避免生成空窗口。
    abstractLength = abstractLength <= 0 ? 150 : abstractLength;

    // 摘要按“字符数”截取，而不是按字节截取。中文 UTF-8 一个字通常占 3 字节，
    // 直接 substr 可能截断一个汉字导致乱码。
    std::string plainText = clean_content(content);
    std::vector<std::string> chars = TextUtils::split_utf8_characters(plainText);
    if (chars.empty()) {
        return "";
    }

    // 预先计算每个关键词的字符序列和出现位置，后续窗口打分可以反复使用。
    std::vector<std::vector<int>> keywordPositions;
    std::vector<std::vector<std::string>> keywordChars;
    for (const auto& keyword : keywords) {
        // const auto& 避免复制 keywords 中的字符串。
        // back() 取得刚刚 push_back 进去的关键词字符数组，再用于查找位置。
        keywordChars.push_back(TextUtils::split_utf8_characters(keyword));
        keywordPositions.push_back(find_keyword_positions(chars, keywordChars.back()));
    }

    // 局部结构体只在 generate() 内部使用，用来保存一个候选摘要窗口。
    struct Window {
        int begin = 0;
        int end = 0;
        double score = 0.0;
    };

    Window best;
    // 如果所有关键词都没有命中，就返回正文开头一段作为兜底摘要。
    best.end = std::min(abstractLength, static_cast<int>(chars.size()));

    // 以每个关键词出现位置为中心生成候选窗口，然后按命中数、覆盖数和位置权重
    // 打分。逻辑保持直接，便于从代码中对应开发文档里的摘要规则。
    for (std::size_t k = 0; k < keywordPositions.size(); ++k) {
        for (int pos : keywordPositions[k]) {
            // 尽量让关键词出现在窗口中部偏前的位置。若靠近文章开头或结尾，
            // 再通过 max/min 把窗口边界限制在 [0, chars.size()] 内。
            int begin = std::max(0, pos - 50);
            int end = std::min(static_cast<int>(chars.size()), begin + abstractLength);
            begin = std::max(0, end - abstractLength);

            int totalHits = 0;
            int coveredKeywords = 0;
            int firstHit = static_cast<int>(chars.size());
            int lastHit = 0;

            // 对当前窗口统计命中总量、覆盖了几个不同关键词，以及命中位置是否集中。
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
                    // 每个被覆盖的关键词给基础分，多次命中给少量增益，避免刷屏式
                    // 高频词完全压过其他关键词。
                    totalHits += 10 + std::min(6, (hitsForKeyword - 1) * 2);
                }
            }

            // 覆盖更多查询词的窗口更符合用户意图；多个关键词靠得近时，摘要更紧凑。
            // 三目运算符 `condition ? a : b` 表示条件成立取 a，否则取 b。
            double coverageBonus = keywords.empty()
                ? 0.0
                : static_cast<double>(coveredKeywords) / keywords.size() * 10.0;
            double closeBonus = (coveredKeywords >= 2 && lastHit - firstHit <= 40) ? 5.0 : 0.0;
            double score = totalHits * position_weight(pos, static_cast<int>(chars.size()))
                         + coverageBonus
                         + closeBonus;

            if (score > best.score) {
                // 使用聚合初始化一次性更新 begin、end、score。
                best = {begin, end, score};
            }
        }
    }

    std::string abstract = join_chars(chars, best.begin, best.end);
    return highlight_keywords(abstract, keywords);
}
