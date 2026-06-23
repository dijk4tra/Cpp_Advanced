#include "../../include/common/Config.h"
#include "../../include/offline/KeywordProcessor.h"
#include "../../include/offline/PageProcessor.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main()
{
    try {
        // 所有相对路径都以程序启动时的工作目录为基准
        // 从项目根目录运行
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
