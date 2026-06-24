#include "../../include/common/Config.h"
#include "../../include/online/KeywordRecommender.h"
#include "../../include/online/SearchServer.h"
#include "../../include/online/WebSearcher.h"

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace
{
std::string get_or(const Config& config, const std::string& key, const std::string& fallback)
{
    try {
        return config.get(key);
    } catch (const std::exception&) {
        return fallback;
    }
}

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
 * @brief 搜索引擎第二期在线服务入口。
 *
 * 程序启动后先加载一期生成的词典、索引和网页库，再启动 muduo TCP 服务。
 * 第二期暂不实现缓存，所有在线查询只读取启动时加载好的共享数据。
 */
int main()
{
    try {
        Config config("conf/config.conf");

        std::cout << "========== SearchEngine V2 Online Server ==========" << std::endl;

        KeywordRecommender recommender;
        recommender.load(config.get("cn_dict"),
                         config.get("cn_dict_index"),
                         config.get("en_dict"),
                         config.get("en_dict_index"));

        WebSearcher searcher;
        searcher.load(config.get("pages"),
                      config.get("offsets"),
                      config.get("invert_index"),
                      config.get("cn_stop_words"));

        std::string ip = get_or(config, "server_ip", "0.0.0.0");
        int port = get_int_or(config, "server_port", 8888);
        int ioThreads = get_int_or(config, "io_threads", 4);
        int keywordTopK = get_int_or(config, "keyword_topk", 5);
        int webTopK = get_int_or(config, "web_topk", 10);
        int maxMessageSize = get_int_or(config, "max_message_size", 1024 * 1024);
        searcher.set_abstract_length(get_int_or(config, "abstract_length", 150));

        muduo::net::EventLoop loop;
        muduo::net::InetAddress listenAddr(ip, static_cast<uint16_t>(port));
        SearchServer server(&loop,
                            listenAddr,
                            recommender,
                            searcher,
                            ioThreads,
                            static_cast<uint32_t>(maxMessageSize),
                            keywordTopK,
                            webTopK);

        server.start();
        std::cout << "[Online] listen on " << ip << ":" << port << std::endl;
        loop.loop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[Error] " << ex.what() << std::endl;
        return 1;
    }
}
