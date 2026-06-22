#include "../../include/common/Config.h"
#include "../../include/offline/KeywordProcessor.h"
#include "../../include/offline/PageProcessor.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main()
{
    try {
        Config config("conf/config.conf");

        // create_directories 的特点是：目录已存在时不会报错。
        // 离线结果分开放到 data/dict 和 data/index，便于二期按类别加载。
        std::filesystem::create_directories("data/dict");
        std::filesystem::create_directories("data/index");
        std::filesystem::create_directories("bin");

        std::cout << "========== SearchEngine V1 Offline Builder ==========" << std::endl;

        // 1. 构建关键字推荐需要的词典库和索引库。
        KeywordProcessor keywordProcessor(config.get("en_stop_words"),
                                          config.get("cn_stop_words"));
        keywordProcessor.process(config.get("cn_corpus_dir"),
                                 config.get("en_corpus_dir"),
                                 config.get("cn_dict"),
                                 config.get("cn_dict_index"),
                                 config.get("en_dict"),
                                 config.get("en_dict_index"));

        // 2. 构建网页搜索需要的网页库、偏移库和倒排索引库。
        PageProcessor pageProcessor(config.get("cn_stop_words"));
        pageProcessor.process(config.get("webpage_corpus_dir"),
                              config.get("pages"),
                              config.get("offsets"),
                              config.get("invert_index"));

        std::cout << "========== Build Finished ==========" << std::endl;
        std::cout << "Output directories: data/dict, data/index" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[Error] " << ex.what() << std::endl;
        return 1;
    }
}
