#pragma once

#include <string>
#include <vector>

/**
 * @brief 提供语料目录扫描功能的无状态工具类。
 *
 * 关键字推荐和网页搜索都会扫描目录，因此将 `opendir`、`readdir`、
 * `closedir` 相关逻辑集中在此处。该类只查看指定目录的直接子项，不递归进入
 * 子目录；返回结果只包含普通文件以及指向普通文件的符号链接。
 */
class DirectoryScanner
{
public:
    /**
     * @brief 扫描目录并返回其中所有普通文件的完整路径。
     *
     * 结果按路径字典序排序，以消除 `readdir` 顺序不确定性，保证相同语料
     * 多次建库时文档顺序稳定。`.`、`..`、子目录和设备文件会被忽略。
     *
     * @param dir 要扫描的目录路径；末尾是否带 `/` 均可。
     * @return 排序后的文件完整路径列表。目录为空时返回空 vector。
     * @throws std::runtime_error `opendir` 无法打开 dir 时抛出，异常消息包含
     *         目录路径和系统错误原因。
     */
    static std::vector<std::string> scan(const std::string& dir);

private:
    /**
     * @brief 禁止构造 DirectoryScanner 实例。
     *
     * `= delete` 是 C++11 语法，表示该函数被明确删除，任何尝试实例化该工具类
     * 的代码都会在编译期报错，而不是运行时才失败。
     */
    DirectoryScanner() = delete;
};
