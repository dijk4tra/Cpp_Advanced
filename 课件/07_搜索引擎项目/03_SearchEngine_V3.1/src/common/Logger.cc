#include "../../include/common/Logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <vector>

namespace
{
/**
 * @brief 将配置文件中的日志级别字符串转换为 spdlog 枚举。
 *
 * 参数按值传递是有意的：函数需要把字符串原地转成小写，复制一份可以保证调用方
 * 的原配置不被修改。无法识别的值统一回退为 info，避免拼写错误导致日志全部关闭。
 */
spdlog::level::level_enum parse_level(std::string level)
{
    // std::transform 的输入和输出区间相同，因此执行原地转换。lambda 参数使用
    // unsigned char，避免负值 char 直接传给 std::tolower 产生未定义行为。
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    // level_enum 是强类型枚举，调用方不能误把任意整数当成日志级别。
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical" || level == "fatal") return spdlog::level::critical;
    if (level == "off") return spdlog::level::off;
    return spdlog::level::info;
}
}

/**
 * @brief 初始化当前进程唯一的异步默认 logger。
 *
 * 初始化顺序不能随意调整：先校验配置和创建目录，再初始化全局线程池，随后创建
 * sinks 和 async_logger，最后替换 spdlog 默认 logger。完成后项目代码可直接调用
 * spdlog::info/warn 等自由函数，无需在每个模块中传递 logger 对象。
 */
void AppLogger::init(const Options& input)
{
    // 复制 Options 后做兜底，不修改调用方传入的只读对象。单行 if 仅用于简单的
    // 默认值修正；这些约束也防止 rotating sink 收到 0 容量等非法参数。
    Options options = input;
    if (options.loggerName.empty()) options.loggerName = "search_engine";
    if (options.filePath.empty()) options.filePath = "logs/search_engine.log";
    if (options.maxFileSizeBytes == 0) options.maxFileSizeBytes = 20U * 1024U * 1024U;
    if (options.maxFiles == 0) options.maxFiles = 5;
    if (options.asyncQueueSize == 0) options.asyncQueueSize = 8192;

    // filesystem::path 能跨平台拆分父目录。create_directories 具有幂等性：目录
    // 已存在时不会报错，不存在时会递归创建所有父目录。
    const std::filesystem::path logPath(options.filePath);
    if (!logPath.parent_path().empty()) {
        std::filesystem::create_directories(logPath.parent_path());
    }

    // spdlog 的异步 logger 共享全局线程池。这里只创建 1 个后台线程，日志写盘按序
    // 执行且足以满足当前项目；前台业务线程只负责把日志消息放入有界队列。
    spdlog::init_thread_pool(options.asyncQueueSize, 1);

    // stdout_color_sink_mt 输出带颜色的线程安全控制台日志；rotating_file_sink_mt
    // 在文件达到 maxFileSizeBytes 后自动轮转，并最多保留 maxFiles 个文件。
    // make_shared 让 logger 和局部变量共享 sink 生命周期，函数返回后 sink 仍有效。
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        options.filePath, options.maxFileSizeBytes, options.maxFiles);

    // sink_ptr 是 shared_ptr<sink> 的别名。用 vector 的迭代器区间构造 logger，表示
    // 同一条消息会同时分发到控制台和滚动文件。
    std::vector<spdlog::sink_ptr> sinks{consoleSink, fileSink};
    auto logger = std::make_shared<spdlog::async_logger>(
        options.loggerName,
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        // 队列满时选择 block，而不是 overrun_oldest。日志洪峰会短暂反压业务线程，
        // 但不会悄悄覆盖 WARN/ERROR；正常 INFO 量已通过聚合和级别控制保持较低。
        spdlog::async_overflow_policy::block);

    // pattern 中 %e 是毫秒、%l 是级别、%t 是线程 id、%v 是正文。WARN 及以上
    // 立即刷新，普通日志每 3 秒刷新，在可排障性和磁盘写入次数之间取平衡。
    logger->set_level(parse_level(options.level));
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [thread %t] %v");
    logger->flush_on(spdlog::level::warn);
    // 替换默认 logger 后，所有 spdlog 自由函数都进入这一套 sink 和异步队列。
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(3));
}

/**
 * @brief 刷新日志、停止后台线程并释放 spdlog 全局资源。
 *
 * 离线进程和启动失败路径会显式调用该函数，保证退出前日志已落盘。在线正常退出
 * 时需先析构 muduo 服务对象，避免它在 logger 已关闭后继续输出，因此只在安全的
 * 生命周期位置调用 shutdown。
 */
void AppLogger::shutdown()
{
    spdlog::shutdown();
}
