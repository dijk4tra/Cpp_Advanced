#pragma once

#include <string>
#include <vector>

// DirectoryScanner 只负责一件事：扫描一个目录，返回目录下所有普通文件的完整路径。
//
// 关键字推荐和网页搜索都会用到目录扫描逻辑，所以把它单独封装，避免在多个
// Processor 中重复写 opendir/readdir/closedir。
class DirectoryScanner
{
public:
    // 扫描 dir 目录，返回排序后的文件路径列表。
    // 返回完整路径，例如：../corpus/EN/english.txt。
    static std::vector<std::string> scan(const std::string& dir);

private:
    DirectoryScanner() = delete;
};
