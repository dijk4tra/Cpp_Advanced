#include "../../include/common/TextUtils.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utfcpp/utf8.h>

namespace
{
// is_useless_token 需要同时识别 ASCII 空白和标点，把这部分判断集中在此处。
// 参数使用 unsigned char，以满足 cctype 系列函数的取值要求。
bool is_ascii_space_or_punct(unsigned char ch)
{
    return std::isspace(ch) || std::ispunct(ch);
}
}

namespace TextUtils
{
std::set<std::string> load_stop_words(const std::string& filename)
{
    // 停用词会参与每个 token 的过滤，文件缺失会显著污染词典和倒排索引，
    // 因此不能静默返回空集合。
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open stop words file: " + filename);
    }

    std::set<std::string> stopWords;
    std::string word;
    // operator>> 按任意空白分隔，所以既支持“一行一个词”，也支持一行多个词。
    // set 同时完成去重，并提供 O(log n) 的稳定查询性能。
    while (ifs >> word) {
        stopWords.insert(word);
    }
    return stopWords;
}

std::string normalize_english_line(const std::string& line)
{
    std::string result;
    // 归一化后的字符串不会比原字符串更长，预留相同容量可减少扩容。
    result.reserve(line.size());

    for (unsigned char ch : line) {
        // 课程要求英文语料只保留字母并统一小写。非字母替换为空格而不是直接
        // 删除，避免 "hello,world" 被错误拼接成 "helloworld"。
        if (std::isalpha(ch)) {
            result.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            result.push_back(' ');
        }
    }

    return result;
}

std::vector<std::string> split_utf8_characters(const std::string& text)
{
    std::vector<std::string> characters;

    // UTF-8 使用变长编码，一个中文字符通常占 3 个字节，不能通过 text[i]
    // 按字节拆分。curr/end 表示当前字符起点和整个字节序列的末尾。
    const char* curr = text.c_str();
    const char* end = text.c_str() + text.size();

    while (curr != end) {
        const char* start = curr;
        // utf8::next 根据当前码点的编码长度移动 curr，并校验 UTF-8 序列。
        // [start, curr) 对应一个完整 Unicode 码点的 UTF-8 字节。
        utf8::next(curr, end);
        characters.emplace_back(start, curr);
    }

    return characters;
}

bool is_useless_token(const std::string& token)
{
    if (token.empty()) {
        return true;
    }

    // 只要 token 中至少含有一个非噪声字符，就保留整个 token；只有 token
    // 完全由空白、标点或 ASCII 数字组成时才判定为无意义。
    bool hasUsefulCharacter = false;
    for (const auto& character : split_utf8_characters(token)) {
        // ASCII 空白、标点和数字都不适合作为中文关键词。
        if (character.size() == 1) {
            unsigned char ch = static_cast<unsigned char>(character[0]);
            if (is_ascii_space_or_punct(ch) || std::isdigit(ch)) {
                continue;
            }
        }

        // cctype 只能直接处理单字节 ASCII 字符，多字节中文标点需显式列出。
        // static 保证集合只初始化一次，避免每次检查 token 时重复构造。
        static const std::set<std::string> punctuations = {
            "，", "。", "！", "？", "；", "：", "、", "“", "”", "‘", "’",
            "（", "）", "【", "】", "《", "》", "—", "…", "￥", "·"
        };
        if (punctuations.count(character) != 0) {
            continue;
        }

        hasUsefulCharacter = true;
    }

    return !hasUsefulCharacter;
}

std::string escape_xml(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    // pages.dat 使用 XML 风格标签保存文档。正文中的 &, <, > 若原样写入，
    // 会被 XML 解析器误认为实体或标签边界。文本节点中的引号无需转义。
    // 必须先按原字符逐个处理，才能避免把新生成实体中的 & 再次转义。
    for (char ch : text) {
        switch (ch) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        default:
            result.push_back(ch);
            break;
        }
    }

    return result;
}
}
