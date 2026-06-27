#include "../../include/common/TextUtils.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utfcpp/utf8.h>

namespace
{
/**
 * @brief 判断单个 ASCII 字节是否为空白或标点。
 * @param ch 要检查的无符号字节。
 * @return 为空白或标点时返回 true，否则返回 false。
 * @note 异常处理：本函数不抛出异常。
 *
 * @note 参数使用 unsigned char，以满足 cctype 系列函数的取值要求。
 */
bool is_ascii_space_or_punct(unsigned char ch)
{
    return std::isspace(ch) || std::ispunct(ch);
}

/**
 * @brief 判断 Unicode 码点是否属于项目接受的汉字范围。
 * @param codePoint 待检查的 Unicode 码点。
 * @return 属于汉字相关区块时返回 true，否则返回 false。
 * @note 异常处理：本函数只比较整数范围，不抛出异常。
 */
bool is_han_code_point(char32_t codePoint)
{
    // U+3007 是中文数字“〇”，它位于 CJK 符号和标点区，不在统一表意文字区。
    if (codePoint == 0x3007) {
        return true;
    }

    // 常用汉字主要位于扩展 A 和基本区，课程语料绝大多数命中这两个范围。
    if ((codePoint >= 0x3400 && codePoint <= 0x4DBF)
        || (codePoint >= 0x4E00 && codePoint <= 0x9FFF)
        || (codePoint >= 0xF900 && codePoint <= 0xFAFF)) {
        return true;
    }

    // 补充 Unicode 辅助平面中的 CJK 扩展 B-I 和兼容表意文字补充区，
    // 避免生僻汉字及兼容字形被误删。
    return (codePoint >= 0x20000 && codePoint <= 0x2A6DF)
        || (codePoint >= 0x2A700 && codePoint <= 0x2B73F)
        || (codePoint >= 0x2B740 && codePoint <= 0x2B81F)
        || (codePoint >= 0x2B820 && codePoint <= 0x2CEAF)
        || (codePoint >= 0x2CEB0 && codePoint <= 0x2EBEF)
        || (codePoint >= 0x2EBF0 && codePoint <= 0x2EE5F)
        || (codePoint >= 0x2F800 && codePoint <= 0x2FA1F)
        || (codePoint >= 0x30000 && codePoint <= 0x3134F)
        || (codePoint >= 0x31350 && codePoint <= 0x323AF);
}
}

namespace TextUtils
{
/**
 * @brief 加载按空白分隔的停用词。
 * @param filename 停用词文件路径。
 * @return 去重后的停用词集合。
 * @throws std::runtime_error 文件无法打开时抛出。
 */
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
    // `ifs >> word` 调用格式化输入，自动跳过前导空白并读取到下一处空白。
    while (ifs >> word) {
        stopWords.insert(word);
    }
    return stopWords;
}

/**
 * @brief 只保留英文字符并统一转换为小写。
 * @param line 原始英文文本行。
 * @return 完成归一化的新字符串。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
std::string normalize_english_line(const std::string& line)
{
    std::string result;
    // 归一化后的字符串不会比原字符串更长，预留相同容量可减少扩容。
    result.reserve(line.size());

    // 范围 for 会按顺序访问 line 的每个字节。这里按值取得 unsigned char，
    // 既不修改原字符串，也能安全传给 std::isalpha/std::tolower。
    for (unsigned char ch : line) {
        // 英文语料只保留字母并统一小写。非字母替换为空格而不是直接删除，
        // 避免 "hello,world" 被错误拼接成 "helloworld"。
        if (std::isalpha(ch)) {
            // 显式将 std::tolower 返回的 int 类型转换为 char，防止隐式缩窄转换引发编译警告。
            result.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            result.push_back(' ');
        }
    }

    return result;
}

/**
 * @brief 将 UTF-8 字符串拆分为独立 Unicode 码点。
 * @param text 合法 UTF-8 文本。
 * @return 每个元素包含一个完整 UTF-8 字符的 vector。
 * @throws utf8::exception 输入编码非法或截断时由 utfcpp 抛出。
 */
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
        // emplace_back 直接调用 string(start, curr) 的区间构造函数，
        // 在 vector 尾部原位构造字符串，避免先创建临时 string 再复制。
        characters.emplace_back(start, curr);
    }

    return characters;
}

/**
 * @brief 判断 token 是否完全由空白、标点或 ASCII 数字组成。
 * @param token 待检查的 UTF-8 token。
 * @return token 无检索意义时返回 true，否则返回 false。
 * @throws utf8::exception token 编码非法时抛出。
 */
bool is_useless_token(const std::string& token)
{
    if (token.empty()) {
        return true;
    }

    // 只要 token 中至少含有一个非噪声字符，就保留整个 token；只有 token
    // 完全由空白、标点或 ASCII 数字组成时才判定为无意义。
    bool hasUsefulCharacter = false;
    // const auto& 让编译器推导元素类型为 std::string，并通过常量引用避免复制。
    for (const auto& character : split_utf8_characters(token)) {
        // ASCII 空白、标点和数字都不适合作为中文关键词。
        if (character.size() == 1) {
            // 只有 size()==1 才按 ASCII 处理；多字节字符不能交给 cctype 判断。
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
        // set::count 对唯一键集合只会返回 0 或 1，可直接判断字符是否存在。
        if (punctuations.count(character) != 0) {
            continue;
        }

        hasUsefulCharacter = true;
    }

    return !hasUsefulCharacter;
}

/**
 * @brief 判断 UTF-8 token 是否非空且完全由汉字组成。
 * @param token 待检查的 UTF-8 token。
 * @return 全部码点都是汉字时返回 true，否则返回 false。
 * @throws utf8::exception token 编码非法或截断时由 utfcpp 抛出。
 */
bool is_chinese_word(const std::string& token)
{
    if (token.empty()) {
        return false;
    }

    // curr 是可移动的当前字节位置。utf8::next 每次返回一个完整 Unicode 码点，
    // 并将 curr 移到下一个字符，因此不会把汉字的多个 UTF-8 字节分别判断。
    const char* curr = token.c_str();
    const char* end = curr + token.size();
    while (curr != end) {
        char32_t codePoint = static_cast<char32_t>(utf8::next(curr, end));
        if (!is_han_code_point(codePoint)) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 转义 XML 文本节点中的 `&`、`<` 和 `>`。
 * @param text 原始文本。
 * @return XML 安全的新字符串。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
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
