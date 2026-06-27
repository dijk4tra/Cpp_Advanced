#pragma once

#include "../cache/CachedSearchService.h"

#include <muduo/net/TcpServer.h>

#include <cstdint>
#include <map>
#include <string>

/**
 * @brief 面向浏览器测试的轻量 HTTP 服务。
 *
 * 该服务与原 TLV SearchServer 运行在同一个进程中，但监听不同端口。HTTP 服务
 * 负责两类请求：
 * 1. GET 静态文件：返回 www/index.html、styles.css、app.js 等前端文件。
 * 2. POST /api/search 或 /api/suggest：直接调用当前进程内的搜索模块。
 *
 * 这里没有引入完整 HTTP 框架，只实现浏览器测试需要的最小 HTTP/1.1 能力。
 * 这样可以让 `./bin/search_server` 同时服务 TLV 客户端和浏览器页面，避免再
 * 单独启动 Python bridge。
 */
class WebHttpServer
{
public:
    /**
     * @brief 构造 HTTP 测试服务器并注册 muduo 回调。
     *
     * @param loop 主事件循环，由 main 创建并负责 loop()。
     * @param listenAddr HTTP 服务监听地址。
     * @param service 带缓存的搜索服务，供 /api/suggest 和 /api/search 使用。
     * @param wwwRoot 前端静态文件根目录。
     * @param httpThreads HTTP 服务的 muduo 工作线程数。
     * @param keywordTopK HTTP 关键字推荐返回数量，来自服务端配置。
     * @param webTopK HTTP 网页搜索返回数量，来自服务端配置。
     * @param maxRequestSize 单条 HTTP 请求头和 body 的最大总字节数。
     * @param slowRequestMs 超过该毫秒数的请求记录 WARN。
     */
    WebHttpServer(muduo::net::EventLoop* loop,
                  const muduo::net::InetAddress& listenAddr,
                  CachedSearchService& service,
                  const std::string& wwwRoot,
                  int httpThreads,
                  int keywordTopK,
                  int webTopK,
                  std::size_t maxRequestSize,
                  int slowRequestMs);

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

        // HTTP/1.1 默认复用连接；Connection: close 或 HTTP/1.0 默认关闭。
        bool keepAlive = false;
    };

private:
    /**
     * @brief 连接建立或断开时打印日志。
     */
    void on_connection(const muduo::net::TcpConnectionPtr& conn);

    /**
     * @brief 处理浏览器发来的 HTTP 字节流。
     *
     * 支持 HTTP/1.1 keep-alive，并循环处理同一个 Buffer 中已经到达的多条请求。
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
                              const std::string& body,
                              bool keepAlive) const;

    /**
     * @brief 组装 JSON 格式的错误响应。
     */
    std::string make_json_error(int statusCode,
                                const std::string& message,
                                bool keepAlive = false) const;

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

    // 与 TLV 服务共享的带缓存业务服务。
    CachedSearchService& service_;

    // 前端静态文件目录，一般为项目根目录下的 www。
    std::string wwwRoot_;

    // HTTP API 返回数量由服务端配置统一控制，浏览器请求不覆盖。
    int keywordTopK_;
    int webTopK_;

    // 防止客户端发送无限增长的请求头或伪造超大 Content-Length。
    std::size_t maxRequestSize_;
    int slowRequestMs_;
};
