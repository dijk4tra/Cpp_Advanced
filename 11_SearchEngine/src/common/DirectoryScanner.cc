#include "../../include/common/DirectoryScanner.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
// 判断 path 最终指向的对象是否为普通文件。
//
// 这里使用 stat 而不是依赖 dirent::d_type，因为部分文件系统会把 d_type
// 返回为 DT_UNKNOWN。stat 会跟随符号链接，所以指向普通文件的符号链接
// 也会作为语料文件被接收
bool is_regular_file(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}
} // end of unnamed namespace

std::vector<std::string> DirectoryScanner::scan(const std::string& dir)
{
    std::vector<std::string> files;

    // 使用目录流接口扫描语料目录
    // 打开失败则抛异常，防止“空语料”被误认为正常结果
    DIR* directory = ::opendir(dir.c_str());
    if (directory == nullptr) {
        throw std::runtime_error("opendir failed: " + dir + ", reason: " + std::strerror(errno));
    }

    while (dirent* entry = ::readdir(directory)) {
        std::string name = entry->d_name;

        // Linux 目录中固定会有 . 和 ..，它们不是语料文件，必须跳过
        if (name == "." || name == "..") {
            continue;
        }

        // scan 对外返回可直接打开的完整路径，
        // 同时兼容调用者传入带或不带末尾斜杠的目录名。
        std::string fullPath = dir;
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
    std::sort(files.begin(), files.end());
    return files;
}
