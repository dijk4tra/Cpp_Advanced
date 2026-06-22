#pragma once

#include <set>
#include <string>
#include <vector>

namespace TextUtils
{
// 读取停用词文件，一行或一个空白分隔项都可以作为一个停用词
std::set<std::string> load_stop_words(const std::string &filename);

// 英文预处理：只保留字母，其余字符全部替换为空格，并统一转成小写
std::string normalize_english_line(const std::string &line);

// 使用 utfcpp 将 UTF-8 字符串切成“一个个字符”
// 例如："搜索" 会切成 ["搜", "索"]，而不是按字节切
std::vector<std::string> split_utf8_characters(const std::string &text);

// 判断中文分词结果是否是无意义 token，例如空白、常见标点、纯数字等。
bool is_useless_token(const std::string& token);

// 将 XML 特殊字符转义，避免正文里出现 < 或 & 时破坏 pages.dat 的标签结构。
std::string escape_xml(const std::string& text);
} // end of namespace TextUtils
