#include "../../include/common/DirectoryScanner.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace
{
// 匿名命名空间限制辅助函数的链接范围，避免与其他翻译单元的同名函数冲突。

/**
 * @brief 判断路径最终指向的对象是否为普通文件。
 *
 * 使用 stat 而不是依赖 `dirent::d_type`，因为部分文件系统会返回
 * `DT_UNKNOWN`。stat 会跟随符号链接，所以指向普通文件的符号链接也会被接收。
 *
 * @param path 要检查的完整路径。
 * @return stat 成功且目标为普通文件时返回 true，否则返回 false。
 * @note 异常处理：本函数不抛出异常，stat 失败会被转换为 false。
 */
bool is_regular_file(const std::string& path)
{
    // struct stat 是 POSIX 定义的元数据结构，stat() 会把查询结果写入 st。
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}
}

/**
 * @brief 扫描目录并返回排序后的普通文件路径。
 * @param dir 要扫描的目录路径。
 * @return 排序后的完整文件路径 vector。
 * @throws std::runtime_error opendir 无法打开目录时抛出。
 */
std::vector<std::string> DirectoryScanner::scan(const std::string& dir)
{
    std::vector<std::string> files;

    // PDF 要求使用目录流接口扫描语料目录。opendir 成功后，必须在函数返回前
    // 与 closedir 成对调用；打开失败则抛异常，防止“空语料”被误认为正常结果。
    // `::opendir` 前的 `::` 强制从全局命名空间查找 POSIX 函数；c_str() 将
    // std::string 暴露为以 '\0' 结尾的 C 字符串。
    DIR* directory = ::opendir(dir.c_str());
    if (directory == nullptr) {
        throw std::runtime_error("opendir failed: " + dir + ", reason: " + std::strerror(errno));
    }

    // C++ 允许在 while 条件中声明变量。readdir 返回 nullptr 时循环结束。
    while (dirent* entry = ::readdir(directory)) {
        std::string name = entry->d_name;

        // Linux 目录中固定会有 . 和 ..，它们不是语料文件，必须跳过。
        if (name == "." || name == "..") {
            continue;
        }

        // scan 对外返回可直接打开的完整路径，同时兼容调用者传入带或不带
        // 末尾斜杠的目录名。
        std::string fullPath = dir;
        // `&&` 具有短路求值特性：字符串为空时不会调用 back()，避免越界。
        if (!fullPath.empty() && fullPath.back() != '/') {
            fullPath += '/';
        }
        fullPath += name;

        // 子目录、设备文件等不会进入结果；指向普通文件的符号链接会被接收。
        if (is_regular_file(fullPath)) {
            files.push_back(fullPath);
        }
    }

    // 目录流不再使用后及时释放其文件描述符。
    ::closedir(directory);

    // readdir 的返回顺序由文件系统决定。排序虽然不是算法必需，但能保证
    // 文档初始编号和各输出文件在相同输入下稳定，方便测试及版本对比。
    // [begin, end) 是标准库常用的左闭右开迭代器区间。
    std::sort(files.begin(), files.end());
    return files;
}
