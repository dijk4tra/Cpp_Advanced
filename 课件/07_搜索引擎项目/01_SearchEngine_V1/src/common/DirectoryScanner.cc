#include "common/DirectoryScanner.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace
{
bool is_regular_file(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}
}

std::vector<std::string> DirectoryScanner::scan(const std::string& dir)
{
    std::vector<std::string> files;

    DIR* directory = ::opendir(dir.c_str());
    if (directory == nullptr) {
        throw std::runtime_error("opendir failed: " + dir + ", reason: " + std::strerror(errno));
    }

    while (dirent* entry = ::readdir(directory)) {
        std::string name = entry->d_name;

        // Linux 目录中固定会有 . 和 ..，它们不是语料文件，必须跳过。
        if (name == "." || name == "..") {
            continue;
        }

        std::string fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != '/') {
            fullPath += '/';
        }
        fullPath += name;

        // 这里只收集普通文件，避免把子目录、软链接等误当作语料处理。
        if (is_regular_file(fullPath)) {
            files.push_back(fullPath);
        }
    }

    ::closedir(directory);

    // 排序不是算法必需，但可以保证每次运行生成文件顺序一致，方便调试和对比。
    std::sort(files.begin(), files.end());
    return files;
}
