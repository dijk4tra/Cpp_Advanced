#pragma once

#include <cstddef>
#include <string>

namespace AppLogger
{
struct Options {
    std::string loggerName;
    std::string filePath;
    std::string level = "info";
    std::size_t maxFileSizeBytes = 20U * 1024U * 1024U;
    std::size_t maxFiles = 5;
    std::size_t asyncQueueSize = 8192;
};

/**
 * @brief 初始化“控制台 + 滚动文件”的异步默认 logger。
 *
 * 同一进程只调用一次。日志文件父目录会自动创建；队列满时阻塞生产者，避免
 * 丢失 WARN/ERROR。默认 WARN 及以上立即 flush，并每 3 秒周期 flush。
 */
void init(const Options& options);

/** @brief 刷新并关闭 spdlog 线程池，程序退出前调用。 */
void shutdown();
}
