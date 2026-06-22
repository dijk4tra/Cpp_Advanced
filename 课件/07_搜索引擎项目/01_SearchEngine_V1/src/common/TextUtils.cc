#include "../../include/common/TextUtils.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utfcpp/utf8.h>

namespace
{
bool is_ascii_space_or_punct(unsigned char ch)
{
    return std::isspace(ch) || std::ispunct(ch);
}
}

namespace TextUtils
{
std::set<std::string> load_stop_words(const std::string& filename)
{
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open stop words file: " + filename);
    }

    std::set<std::string> stopWords;
    std::string word;
    while (ifs >> word) {
        stopWords.insert(word);
    }
    return stopWords;
}

std::string normalize_english_line(const std::string& line)
{
    std::string result;
    result.reserve(line.size());

    for (unsigned char ch : line) {
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

    const char* curr = text.c_str();
    const char* end = text.c_str() + text.size();

    while (curr != end) {
        const char* start = curr;
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

    bool hasUsefulCharacter = false;
    for (const auto& character : split_utf8_characters(token)) {
        // ASCII 空白、标点和数字都不适合作为中文关键词。
        if (character.size() == 1) {
            unsigned char ch = static_cast<unsigned char>(character[0]);
            if (is_ascii_space_or_punct(ch) || std::isdigit(ch)) {
                continue;
            }
        }

        // 常见中文标点。这里保留一个小集合，够覆盖语料清洗中的主要噪声。
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
