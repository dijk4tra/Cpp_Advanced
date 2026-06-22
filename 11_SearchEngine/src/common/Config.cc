#include "../../include/common/Config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

// 匿名命名空间: 让里面的函数、变量、类只在当前 .cc 文件内部可见，其他 .cc 文件不能访问
namespace
{
// 去掉配置项目首尾的空白字符
// 配置文件允许写成 "key = value"。如果不先清理空白，最终保存的 key
// 可能会变成 "key "，后续通过 Config::get("key") 就无法找到它。
std::string trim(const std::string& text)
{
    // cctype 系列函数要求参数能够表示为 unsigned char，直接传入有符号 char
    // 在遇到非 ASCII 字节时可能产生未定义行为。
    // std::find_if_not: 找到第一个“不满足条件”的元素, 条件是 isspace(ch)
    // 找到第一个不是空白字符的位置
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });

    // 先用反向迭代器找到最后一个非空白字符，再通过 base() 转回正向迭代器。
    // 注意: 反向迭代器.base() 后，会指向当前字符的后一个正向位置。
    // 得到的 end 指向有效内容末尾的下一个位置, 正好满足 string 的区间构造要求.
    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    // 全空白字符串中, 正向起点会越过反向计算出的终点, 需要单独返回空串
    if (begin >= end) {
        // 如果起点已经不在终点前面，就说明没有有效内容，直接返回空字符串
        return "";
    }
    return std::string(begin, end);
}
}

Config::Config(const std::string& filename)
{
    // 配置决定所有语料和输出文件的位置。打开失败时立即终止建库，
    // 避免后续模块拿着空路径继续执行并产生更难定位的错误。
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open config file: " + filename);
    }

    std::string line;
    int lineNo = 0;
    while (getline(ifs, line)) {
        ++lineNo;

        std::string content = trim(line);
        // 支持空行和以 # 开头的整行注释。当前简单格式不处理行尾注释，
        // 因此 value 中出现 # 时会被当作普通内容保留。
        if (content.empty() || content[0] == '#') {
            continue;
        }

        // 只使用第一个等号分隔 key 和 value，因此 value 本身仍可以包含等号.
        std::size_t pos = content.find('=');
        if (pos == std::string::npos) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        std::string key = trim(content.substr(0, pos));
        std::string value = trim(content.substr(pos + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        // map 的下标赋值会使后出现的同名配置覆盖前面的值。
        items_[key] = value;
    }
}

const std::string& Config::get(const std::string &key) const
{
    auto it = items_.find(key);
    if (it == items_.end()) {
        // 缺少必需配置属于启动阶段错误，抛异常后由 main 统一打印并退出
        throw std::runtime_error("missing config key: " + key);
    }

    // 返回 map 内部字符串的常量引用，避免复制；只要 Config 对象仍然存活，
    // 且 items_ 没有被修改，该引用就保持有效。
    return it->second;
}
