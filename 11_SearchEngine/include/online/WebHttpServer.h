#pragma once

#include "KeywordRecommender.h"
#include "WebSearcher.h"

#include <muduo/net/TcpServer.h>

#include <cstdint>
#include <map>
#include <string>

/**
 * @brief 面向浏览器测试的轻量 HTTP 服务。
 *
 * 该服务与原 TLV SearchServer 运行在同一个进程中，但监听不同端口。
 * HTTP 服务负责两类请求：
 * 1. GET 静态文件：返回 www/index.html、styles.css、app.js 等前端文件。
 * 2. POST /api/search 或 /api/suggest：直接调用当前进程内的搜索模块。
 *
 * 这里没有引入完整 HTTP 框架，只实现浏览器测试需要的最小 HTTP/1.1 能力。
 * 这样可以让 `./bin/search_server` 同时服务 TLV 客户端和浏览器页面。
 */
class WebHttpServer
{
public:
    /**
     * @brief 构造 HTTP 测试服务器并注册 muduo 回调。
     *
     * @param loop 主事件循环，由 main 创建并负责 loop()。
     * @param listenAddr HTTP 服务监听地址。
     * @param recommender 关键字推荐模块引用，供 /api/suggest 使用。
     * @param searcher 网页搜索模块引用，供 /api/search 使用。
     * @param wwwRoot 前端静态文件根目录。
     * @param httpThreads HTTP 服务的 muduo 工作线程数。
     * @param keywordTopK 关键字推荐默认返回数量。
     * @param webTopK 网页搜索默认返回数量。
     */
    WebHttpServer(muduo::net::EventLoop* loop,
                  const muduo::net::InetAddress& listenAddr,
                  KeywordRecommender& recommender,
                  WebSearcher& searcher,
                  const std::string& wwwRoot,
                  int httpThreads,
                  int keywordTopK,
                  int webTopK);

    /**
     * @brief 启动 HTTP 服务。
     */
    void start();

private:
    /**
     * @brief 简化版 HTTP 请求对象。
     *
     * 当前只保存浏览器测试需要的字段：方法、路径、请求头和 body。请求头 key
     * 在解析时统一转成小写，方便查询 content-length。
     */
    struct HttpRequest {
        // HTTP 方法，例如 GET、HEAD、POST。
        std::string method;

        // URL 路径，不包含 query string。
        std::string path;

        // 小写 header key -> header value。
        std::map<std::string, std::string> headers;

        // 请求体。API 请求中保存 JSON 字符串。
        std::string body;
    };

private:
    /**
     * @brief 连接建立或断开时打印日志。
     */
    void on_connection(const muduo::net::TcpConnectionPtr& conn);

    /**
     * @brief 处理浏览器发来的 HTTP 字节流。
     *
     * 当前服务采用短连接，成功响应后主动 shutdown，简化 keep-alive 和多请求
     * 管线化处理。
     */
    void on_message(const muduo::net::TcpConnectionPtr& conn,
                    muduo::net::Buffer* buffer,
                    muduo::Timestamp receiveTime);

    /**
     * @brief 尝试从 Buffer 中解析一条完整 HTTP 请求。
     * @return 成功解析完整请求时返回 true；数据不足时返回 false。
     * @throws std::runtime_error 请求行或 Content-Length 非法时可能抛出。
     */
    bool try_parse_request(muduo::net::Buffer* buffer, HttpRequest& request) const;

    /**
     * @brief 根据 path 分发到 API 或静态文件处理。
     */
    std::string handle_request(const HttpRequest& request) const;

    /**
     * @brief 处理 `/api/suggest` 或 `/api/search`。
     * @param keywordMode true 表示关键字推荐，false 表示网页搜索。
     */
    std::string handle_api(const HttpRequest& request, bool keywordMode) const;

    /**
     * @brief 处理前端静态资源请求。
     */
    std::string handle_static_file(const HttpRequest& request) const;

    /**
     * @brief 组装 HTTP 响应文本。
     */
    std::string make_response(int statusCode,
                              const std::string& statusText,
                              const std::string& contentType,
                              const std::string& body) const;

    /**
     * @brief 组装 JSON 格式的错误响应。
     */
    std::string make_json_error(int statusCode, const std::string& message) const;

    /**
     * @brief 从 wwwRoot_ 中读取静态文件并推断 Content-Type。
     * @param requestPath 浏览器请求路径。
     * @param contentType 输出参数，写入 MIME 类型。
     * @return 文件内容。
     * @throws std::runtime_error 路径非法或文件不存在时抛出。
     */
    std::string load_file(const std::string& requestPath, std::string& contentType) const;

private:
    // muduo TCP 服务对象，HTTP 端口和 TLV 端口分别由两个 TcpServer 监听。
    muduo::net::TcpServer server_;

    // 与 TLV 服务共享的业务模块，避免重复加载词典、网页库和倒排索引。
    KeywordRecommender& recommender_;
    WebSearcher& searcher_;

    // 前端静态文件目录，一般为项目根目录下的 www。
    std::string wwwRoot_;

    // 默认返回数量；POST body 中传 topk 时可覆盖。
    int keywordTopK_;
    int webTopK_;
};
