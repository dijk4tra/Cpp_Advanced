#include "../../include/common/Config.h"
#include "../../include/online/KeywordRecommender.h"
#include "../../include/online/SearchServer.h"
#include "../../include/online/WebHttpServer.h"
#include "../../include/online/WebSearcher.h"

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace
{
// 配置读取辅助函数放在匿名命名空间中，表示它们只属于当前程序入口文件。

/**
 * @brief 读取字符串配置项，不存在时返回默认值。
 *
 * Config::get 对缺失 key 会抛异常。在线服务部分配置允许有默认值，因此在入口
 * 处用该函数统一兜底。
 */
std::string get_or(const Config& config, const std::string& key, const std::string& fallback)
{
    try {
        // Config::get 返回内部字符串的 const 引用；这里按值返回，调用方拿到独立副本。
        return config.get(key);
    } catch (const std::exception&) {
        // 缺少可选配置项时不终止程序，使用调用方提供的默认值。
        return fallback;
    }
}

/**
 * @brief 读取整数配置项，不存在或格式非法时返回默认值。
 */
int get_int_or(const Config& config, const std::string& key, int fallback)
{
    try {
        // stoi 将字符串配置转成 int。配置不存在或不是合法整数都会进入 catch。
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
        // 统一从项目根目录下的 conf/config.conf 读取路径、端口和线程数。
        // Config 是栈对象，main 结束时自动析构。
        Config config("conf/config.conf");

        std::cout << "========== SearchEngine V2 Online Server ==========" << std::endl;

        // 关键字推荐依赖一期离线生成的中英文词典和字符索引。
        KeywordRecommender recommender;
        // config.get() 查不到必需路径时会抛异常，直接进入 catch 并退出程序。
        recommender.load(config.get("cn_dict"),
                         config.get("cn_dict_index"),
                         config.get("en_dict"),
                         config.get("en_dict_index"));

        // 网页搜索依赖网页库、偏移库、倒排索引和中文停用词。
        WebSearcher searcher;
        searcher.load(config.get("pages"),
                      config.get("offsets"),
                      config.get("invert_index"),
                      config.get("cn_stop_words"));

        std::string ip = get_or(config, "server_ip", "0.0.0.0");
        int port = get_int_or(config, "server_port", 8888);
        int httpPort = get_int_or(config, "http_port", 18888);
        int ioThreads = get_int_or(config, "io_threads", 4);
        int httpThreads = get_int_or(config, "http_threads", 2);
        int keywordTopK = get_int_or(config, "keyword_topk", 10);
        int webTopK = get_int_or(config, "web_topk", 33);
        int maxMessageSize = get_int_or(config, "max_message_size", 1024 * 1024);
        std::string wwwRoot = get_or(config, "www_root", "www");
        // 摘要长度属于 WebSearcher 的运行参数，加载索引后再设置即可。
        searcher.set_abstract_length(get_int_or(config, "abstract_length", 150));

        // 两个服务器共用同一个 EventLoop：
        // 1. SearchServer 监听 TLV 端口，供课程协议客户端使用；
        // 2. WebHttpServer 监听 HTTP 端口，供浏览器页面使用。
        muduo::net::EventLoop loop;
        // InetAddress 的端口类型是 uint16_t，因此把 int 端口显式转换为 16 位无符号整数。
        muduo::net::InetAddress listenAddr(ip, static_cast<uint16_t>(port));
        SearchServer server(&loop,
                            listenAddr,
                            recommender,
                            searcher,
                            ioThreads,
                            // maxMessageSize 配置读出是 int，ProtocolCodec 需要 uint32_t。
                            static_cast<uint32_t>(maxMessageSize),
                            keywordTopK,
                            webTopK);

        muduo::net::InetAddress httpListenAddr(ip, static_cast<uint16_t>(httpPort));
        WebHttpServer httpServer(&loop,
                                 httpListenAddr,
                                 recommender,
                                 searcher,
                                 wwwRoot,
                                 httpThreads,
                                 keywordTopK,
                                 webTopK);

        server.start();
        httpServer.start();
        std::cout << "[Online] TLV listen on " << ip << ":" << port << std::endl;
        std::cout << "[Online] Web listen on http://" << ip << ":" << httpPort << std::endl;
        // loop() 进入 muduo 事件循环，之后由回调函数处理连接和请求。
        loop.loop();
        return 0;
    } catch (const std::exception& ex) {
        // 启动阶段任何异常都在这里统一打印，返回非 0 表示程序启动失败。
        std::cerr << "[Error] " << ex.what() << std::endl;
        return 1;
    }
}
