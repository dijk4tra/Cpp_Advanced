#pragma once

#include <set>
#include <string>
#include <vector>

namespace TextUtils
{
/**
 * @brief 从文本文件加载停用词集合。
 *
 * 文件内容按任意空白字符分隔，因此既支持每行一个词，也支持一行多个词。
 * `std::set` 会自动去重。
 *
 * @param filename 停用词文件路径。
 * @return 去重后的停用词集合。
 * @throws std::runtime_error 文件无法打开时抛出。
 */
std::set<std::string> load_stop_words(const std::string& filename);

/**
 * @brief 按课程规则归一化一行英文语料。
 *
 * 字母统一转为小写，数字、标点及其他非字母字符全部替换为空格。使用空格
 * 替换而非删除，可避免标点两侧的单词被错误拼接。
 *
 * @param line 待处理的原始文本行。
 * @return 与输入等长的归一化字符串，可继续使用流提取运算符按空白分词。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
std::string normalize_english_line(const std::string& line);

/**
 * @brief 按 Unicode 码点拆分 UTF-8 字符串。
 *
 * 每个返回元素保存一个完整码点对应的 UTF-8 字节序列。例如 `"搜索"`
 * 会得到 `["搜", "索"]`，不会把一个汉字的多个字节拆开。
 *
 * @param text 编码正确的 UTF-8 字符串。
 * @return 按原顺序排列的 UTF-8 字符 vector；输入为空时返回空 vector。
 * @throws utf8::exception 输入包含截断或非法 UTF-8 序列时，由 utfcpp 抛出。
 */
std::vector<std::string> split_utf8_characters(const std::string& text);

/**
 * @brief 判断分词结果是否完全由无检索意义的字符组成。
 *
 * 空字符串，以及仅包含 ASCII 空白、ASCII 标点、ASCII 数字或内置中文标点
 * 的 token 会被判为无意义。只要包含至少一个其他字符，整个 token 就会保留。
 *
 * @param token 待检查的 UTF-8 分词结果。
 * @return 无意义时返回 true，否则返回 false。
 * @throws utf8::exception token 不是合法 UTF-8 字符串时，由拆分函数抛出。
 */
bool is_useless_token(const std::string& token);

/**
 * @brief 判断 token 是否为仅由汉字组成的中文词语。
 *
 * 函数按 Unicode 码点判断，而不是按 UTF-8 字节判断。支持基本中日韩统一表意
 * 文字、扩展 A-I、兼容表意文字以及中文数字 `〇`。英文、数字、标点、圈号、
 * 罗马数字和中英混合 token 均返回 false。
 *
 * @param token 待检查的 UTF-8 分词结果。
 * @return token 非空且每个 Unicode 码点都是汉字时返回 true，否则返回 false。
 * @throws utf8::exception token 不是合法 UTF-8 字符串时，由 utfcpp 抛出。
 */
bool is_chinese_word(const std::string& token);

/**
 * @brief 转义 XML 文本节点中的特殊字符。
 *
 * 将 `&`、`<`、`>` 分别转换为对应实体，防止网页字段破坏 pages.dat 的标签
 * 结构。函数用于元素文本，不需要转义只在属性值中有特殊意义的引号。
 *
 * @param text 原始文本。
 * @return 完成 XML 实体转义的新字符串。
 * @throws std::bad_alloc 构造结果字符串时内存分配失败可能抛出。
 */
std::string escape_xml(const std::string& text);
}
