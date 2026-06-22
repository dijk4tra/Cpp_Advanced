#include "../../include/common/Config.h"
#include "../../include/offline/KeywordProcessor.h"
#include "../../include/offline/PageProcessor.h"

#include <exception>
#include <filesystem>
#include <iostream>

/**
 * @brief 搜索引擎第一期离线建库程序入口。
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
        // 01_SearchEngine_V1 项目根目录运行，因此这里可以直接读取 conf/config.conf。
        Config config("conf/config.conf");

        // create_directories 的特点是：目录已存在时不会报错。
        // 离线结果分开放到 data/dict 和 data/index，便于二期按类别加载；
        // 若父目录不存在，create_directories 也会一并创建。
        std::filesystem::create_directories("data/dict");
        std::filesystem::create_directories("data/index");
        std::filesystem::create_directories("bin");

        std::cout << "========== SearchEngine V1 Offline Builder ==========" << std::endl;

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

        // 2. 构建网页搜索需要的网页库、偏移库和倒排索引库。
        // 网页建库必须按“提取 -> 去重 -> 网页/偏移库 -> 倒排索引”的顺序执行，
        // 这样三个输出中的文档 id 都对应同一批去重后的连续编号文档。
        PageProcessor pageProcessor(config.get("cn_stop_words"));
        pageProcessor.process(config.get("webpage_corpus_dir"),
                              config.get("pages"),
                              config.get("offsets"),
                              config.get("invert_index"));

        std::cout << "========== Build Finished ==========" << std::endl;
        std::cout << "Output directories: data/dict, data/index" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        // 各模块通过异常上报配置缺失、文件打不开等不可恢复错误。入口统一转换
        // 为错误日志和非零退出码，便于脚本或 CI 判断离线建库是否成功。
        // 按 const 引用捕获可避免异常对象复制和对象切片；what() 返回错误文本。
        std::cerr << "[Error] " << ex.what() << std::endl;
        return 1;
    }
}
