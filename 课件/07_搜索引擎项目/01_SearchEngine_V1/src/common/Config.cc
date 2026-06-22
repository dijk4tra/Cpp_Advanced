#include "../../include/common/Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

// 匿名命名空间: 让里面的函数、变量、类只在当前 .cc 文件内部可见，其他 .cc 文件不能访问
namespace
{
// 匿名命名空间中的名称只在当前 .cc 文件可见，适合放置不属于公共接口的辅助函数。

/**
 * @brief 删除字符串首尾的空白字符。
 *
 * 配置文件允许写成 `key = value`。如果不先清理空白，最终保存的 key 可能
 * 变成 `"key "`，后续通过 `Config::get("key")` 就无法找到它。
 *
 * @param text 待清理的原始字符串。
 * @return 去除首尾空白后的新字符串；输入全为空白时返回空字符串。
 * @throws std::bad_alloc 构造返回字符串时内存分配失败可能抛出。
 */
std::string trim(const std::string& text)
{
    // cctype 系列函数要求参数能够表示为 unsigned char，直接传入有符号 char
    // 在遇到非 ASCII 字节时可能产生未定义行为。
    // lambda 表达式 `[](unsigned char ch) { ... }` 是一个匿名函数。
    // 空捕获列表 [] 表示它不访问外部局部变量。
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });

    // 先用反向迭代器找到最后一个非空白字符，再通过 base() 转回正向迭代器。
    // 注意: 反向迭代器.base() 后，会指向当前字符的后一个正向位置。
    // 得到的 end 指向有效内容末尾的下一个位置，正好满足 string 的区间构造要求。
    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    // 全空白字符串中，正向起点会越过反向计算出的终点，需要单独返回空串。
    if (begin >= end) {
        // 如果起点已经不在终点前面，就说明没有有效内容，直接返回空字符串
        return "";
    }
    return std::string(begin, end);
}
}

/**
 * @brief 打开并解析 `key=value` 配置文件。
 * @param filename 配置文件路径。
 * @throws std::runtime_error 文件无法打开，或有效配置行格式非法时抛出。
 */
Config::Config(const std::string& filename)
{
    // 配置决定所有语料和输出文件的位置。打开失败时立即终止建库，
    // 避免后续模块拿着空路径继续执行并产生更难定位的错误。
    // ifstream 使用 RAII 管理文件：构造时打开，离开函数作用域时析构并自动关闭。
    std::ifstream ifs(filename);
    if (!ifs) {
        throw std::runtime_error("failed to open config file: " + filename);
    }

    std::string line;
    int lineNo = 0;
    // getline 每次读取一整行并丢弃换行符；
    // std::getline 返回流对象的引用，该流在 while 条件中会自动转换为 bool 值。
    // 读取成功时为 true；当读到文件末尾(EOF)或发生错误时变为 false，循环优雅终止。
    while (std::getline(ifs, line)) {
        ++lineNo;

        std::string content = trim(line);
        // 支持空行和以 # 开头的整行注释。当前简单格式不处理行尾注释，
        // 因此 value 中出现 # 时会被当作普通内容保留。
        if (content.empty() || content[0] == '#') {
            continue;
        }

        // 只使用第一个等号分隔 key 和 value，因此 value 本身仍可以包含等号。
        std::size_t pos = content.find('=');
        if (pos == std::string::npos) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        // substr(pos, count) 提取子串；省略 count 时一直取到字符串末尾。
        std::string key = trim(content.substr(0, pos));
        std::string value = trim(content.substr(pos + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("invalid config line " + std::to_string(lineNo) + ": " + line);
        }

        // map 的下标赋值使后出现的同名配置覆盖前面的值。
        // 当前配置文件没有重复项，这一行为也让配置覆盖规则保持明确。
        items_[key] = value;
    }
}

/**
 * @brief 获取指定 key 对应的配置值。
 * @param key 配置项名称。
 * @return items_ 内部 value 的常量引用。
 * @throws std::runtime_error key 不存在时抛出。
 */
const std::string& Config::get(const std::string& key) const
{
    // find 不会像 operator[] 那样在 key 不存在时插入默认值，适合只读查询。
    auto it = items_.find(key);
    if (it == items_.end()) {
        // 缺少必需配置属于启动阶段错误，抛异常后由 main 统一打印并退出。
        throw std::runtime_error("missing config key: " + key);
    }

    // 返回 map 内部字符串的常量引用，避免复制；只要 Config 对象仍然存活，
    // 且 items_ 没有被修改，该引用就保持有效。
    return it->second;
}
