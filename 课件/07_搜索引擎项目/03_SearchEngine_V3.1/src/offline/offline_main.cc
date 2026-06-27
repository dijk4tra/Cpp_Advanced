#include "../../include/common/Config.h"
#include "../../include/common/Logger.h"
#include "../../include/offline/KeywordProcessor.h"
#include "../../include/offline/PageProcessor.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace
{
/** @brief 读取可选字符串配置，配置缺失时使用默认值。 */
std::string get_or(const Config& config, const std::string& key, const std::string& fallback)
{
    try {
        return config.get(key);
    } catch (const std::exception&) {
        return fallback;
    }
}

/**
 * @brief 读取可选整数配置。
 * @note Config::get 缺失和 stoi 格式错误都会抛异常，这里统一回退，日志配置错误
 *       不应阻止核心离线建库流程启动。
 */
int get_int_or(const Config& config, const std::string& key, int fallback)
{
    try {
        return std::stoi(config.get(key));
    } catch (const std::exception&) {
        return fallback;
    }
}
}

/**
 * @brief 搜索引擎 V3.1 离线建库程序入口。
 *
 * 程序从固定位置读取配置，创建输出目录，依次构建关键字推荐数据和网页搜索
 * 数据。所有模块抛出的标准异常都在此处转换为错误日志和非零进程退出码。
 *
 * @return 全部离线数据成功生成时返回 0；捕获到 std::exception 时返回 1。
 * @throws 不向调用环境传播 std::exception；非标准异常不在本函数处理范围内。
 */
int main()
{
    // try 块包围完整建库流程，使任何阶段失败都统一进入 catch，后续阶段不再执行。
    try {
        // 所有相对路径都以程序启动时的工作目录为基准。README 约定从
        // 03_SearchEngine_V3.1 项目根目录运行，因此这里可以直接读取 conf/config.conf。
        Config config("conf/config.conf");

        // 日志目录和级别允许配置；容量参数先用 max 设置下限，再显式转换成
        // size_t。这样不会把负 int 隐式转换为极大的无符号数。
        AppLogger::Options logOptions;
        logOptions.loggerName = "offline_builder";
        logOptions.filePath = get_or(config, "log_dir", "logs") + "/offline_builder.log";
        logOptions.level = get_or(config, "log_level", "info");
        logOptions.maxFileSizeBytes = static_cast<std::size_t>(
            std::max(1, get_int_or(config, "log_max_file_size_mb", 20))) * 1024U * 1024U;
        logOptions.maxFiles = static_cast<std::size_t>(
            std::max(1, get_int_or(config, "log_max_files", 5)));
        logOptions.asyncQueueSize = static_cast<std::size_t>(
            std::max(128, get_int_or(config, "log_async_queue_size", 8192)));
        AppLogger::init(logOptions);
        // steady_clock 不受系统时间校准影响，适合计算阶段耗时；日志显示时间则由
        // spdlog 使用系统时钟生成，两种时钟各司其职。
        const auto buildStarted = std::chrono::steady_clock::now();

        // create_directories 的特点是：目录已存在时不会报错。
        // 离线结果分开放到 data/dict 和 data/index，便于二期按类别加载；
        // 若父目录不存在，create_directories 也会一并创建。
        std::filesystem::create_directories("data/dict");
        std::filesystem::create_directories("data/index");
        std::filesystem::create_directories("bin");

        spdlog::info("offline build started corpus_dir={}",
                     config.get("webpage_corpus_dir"));

        // 1. 构建关键字推荐需要的词典库和索引库。
        // Processor 构造时一次性加载停用词和 Jieba 词典，后续四个步骤复用，
        // 避免为每个文件重复执行代价较高的初始化。
        // 栈对象采用 RAII：main 返回或异常展开时，Processor 成员会自动析构。
        KeywordProcessor keywordProcessor(config.get("en_stop_words"),
                                          config.get("cn_stop_words"));
        keywordProcessor.process(config.get("cn_corpus_dir"),
                                 config.get("en_corpus_dir"),
                                 config.get("cn_dict"),
                                 config.get("cn_dict_index"),
                                 config.get("en_dict"),
                                 config.get("en_dict_index"));

        // 2. 构建网页库、偏移库、BM25 倒排索引和文档长度统计库。
        // 网页建库必须按“提取 -> 去重 -> 网页/偏移库 -> BM25 索引/统计”的顺序执行，
        // 这样四个输出中的文档 id 都对应同一批去重后的连续编号文档。
        PageProcessor pageProcessor(config.get("cn_stop_words"));
        pageProcessor.process(config.get("webpage_corpus_dir"),
                              config.get("pages"),
                              config.get("offsets"),
                              config.get("invert_index"),
                              config.get("bm25_doc_stats"));

        // duration_cast 把 steady_clock 的原生 duration 明确换算为毫秒整数，避免
        // 直接输出平台相关的 tick 单位。
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted).count();
        spdlog::info("offline build finished elapsed_ms={} output_dirs=data/dict,data/index",
                     elapsedMs);
        AppLogger::shutdown();
        return 0;
    } catch (const std::exception& ex) {
        // 各模块通过异常上报配置缺失、文件打不开等不可恢复错误。入口统一转换
        // 为错误日志和非零退出码，便于脚本或 CI 判断离线建库是否成功。
        // 按 const 引用捕获可避免异常对象复制和对象切片；what() 返回错误文本。
        spdlog::critical("offline build failed error={}", ex.what());
        AppLogger::shutdown();
        return 1;
    }
}
