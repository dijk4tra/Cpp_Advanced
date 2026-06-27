#include "../../include/online/WebHttpServer.h"

#include <muduo/base/Logging.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpConnection.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

// 允许 std::bind 中直接使用 _1、_2、_3 作为回调参数占位符。
using namespace std::placeholders;

namespace
{
// HTTP 相关的小工具函数只在本文件内部使用，因此放入匿名命名空间。

/**
 * @brief 返回字符串的小写副本。
 *
 * HTTP 头字段名大小写不敏感，解析时统一转小写，后续查找 header 更简单。
 */
std::string lower_copy(std::string text)
{
    // std::transform 会遍历 [begin, end) 区间，把 lambda 返回值写回第三个参数
    // 指定的位置。这里输入和输出都是 text 本身，表示原地转换。
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        // std::tolower 返回 int，push/赋值给 char 前显式转换，避免编译器警告。
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

/**
 * @brief 删除字符串首尾空白。
 *
 * 该函数用于清理 HTTP header 的 key 和 value。中间空白保持原样，避免误改
 * 合法 header 内容。
 */
std::string trim(std::string text)
{
    // find_if_not 找到第一个“不满足空白条件”的字符，即有效内容起点。
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    // rbegin/rend 是反向迭代器。base() 会转换回普通迭代器，指向有效内容末尾的后一位。
    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

/**
 * @brief 根据状态码返回简短状态文本。
 */
std::string status_text(int statusCode)
{
    switch (statusCode) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

/**
 * @brief 判断字符串是否以指定后缀结尾。
 */
bool has_suffix(const std::string& text, const std::string& suffix)
{
    // 先判断长度，避免 text.size() - suffix.size() 在长度不足时发生无符号下溢。
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * @brief 根据文件后缀推断 HTTP Content-Type。
 *
 * 当前 www 目录只需要支持 html、css、js、json，其他文件统一作为二进制返回。
 */
std::string guess_content_type(const std::string& path)
{
    if (has_suffix(path, ".html")) {
        return "text/html; charset=utf-8";
    }
    if (has_suffix(path, ".css")) {
        return "text/css; charset=utf-8";
    }
    if (has_suffix(path, ".js")) {
        return "application/javascript; charset=utf-8";
    }
    if (has_suffix(path, ".json")) {
        return "application/json; charset=utf-8";
    }
    return "application/octet-stream";
}
}

/**
 * @brief 构造浏览器 HTTP 服务并注册 muduo 回调。
 */
WebHttpServer::WebHttpServer(muduo::net::EventLoop* loop,
                             const muduo::net::InetAddress& listenAddr,
                             CachedSearchService& service,
                             const std::string& wwwRoot,
                             int httpThreads,
                             int keywordTopK,
                             int webTopK)
    // server_ 构造时就绑定 EventLoop、监听地址和服务名。
    : server_(loop, listenAddr, "WebHttpServer")
    // service_ 是引用成员，必须在初始化列表中绑定到外部对象。
    , service_(service)
    , wwwRoot_(wwwRoot)
    , keywordTopK_(keywordTopK)
    , webTopK_(webTopK)
{
    // 与 TLV 服务一样，HTTP 服务也基于 muduo TcpServer；区别只是上层协议解析。
    // 成员函数作为回调时，需要通过 std::bind 绑定 this 指针。
    server_.setConnectionCallback(std::bind(&WebHttpServer::on_connection, this, _1));
    server_.setMessageCallback(std::bind(&WebHttpServer::on_message, this, _1, _2, _3));
    // HTTP 静态文件和 API 请求使用独立线程数，不影响 TLV 服务的线程配置。
    server_.setThreadNum(httpThreads);
}

/**
 * @brief 启动 HTTP TcpServer。
 */
void WebHttpServer::start()
{
    // start() 开始监听端口，真正的网络事件由 main 中的 EventLoop::loop() 驱动。
    server_.start();
}

/**
 * @brief 打印 HTTP 客户端连接状态。
 */
void WebHttpServer::on_connection(const muduo::net::TcpConnectionPtr& conn)
{
    // connected() 为 true 表示新连接建立；为 false 表示连接断开。
    if (conn->connected()) {
        LOG_INFO << "http client connected: " << conn->peerAddress().toIpPort();
    } else {
        LOG_INFO << "http client disconnected: " << conn->peerAddress().toIpPort();
    }
}

/**
 * @brief 处理 HTTP 请求。
 */
void WebHttpServer::on_message(const muduo::net::TcpConnectionPtr& conn,
                               muduo::net::Buffer* buffer,
                               muduo::Timestamp)
{
    try {
        HttpRequest request;
        if (!try_parse_request(buffer, request)) {
            // HTTP 请求头或 body 尚未完整到达，保留 Buffer 内容等待下一次回调。
            return;
        }

        // 当前实现采用短连接响应。这样不需要处理 keep-alive、多请求管线化等复杂
        // HTTP 行为，更适合课程项目里作为浏览器测试入口。
        // send() 只是把数据交给 muduo 发送缓冲区，不要求当前函数阻塞等待发送完成。
        conn->send(handle_request(request));
        // shutdown() 表示发送完响应后关闭写端。浏览器看到 Connection: close 后也会关闭连接。
        conn->shutdown();
    } catch (const std::exception& ex) {
        // JSON 解析失败、路径非法、Content-Length 非法等异常统一返回 400。
        conn->send(make_json_error(400, ex.what()));
        conn->shutdown();
    }
}

/**
 * @brief 从 muduo Buffer 中尝试解析完整 HTTP 请求。
 */
bool WebHttpServer::try_parse_request(muduo::net::Buffer* buffer, HttpRequest& request) const
{
    // muduo Buffer 只保证字节连续可读，不保证刚好是一条 HTTP 请求。这里先复制
    // 当前可读内容用于查找请求头结束标记。
    // peek() 不移动读指针，只有确认完整请求后才会 retrieve()。
    std::string raw(buffer->peek(), buffer->readableBytes());
    std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        // 连 HTTP 头都没收完整，继续等待后续数据。
        return false;
    }

    // headerEnd 指向 "\r\n\r\n" 的起始位置，substr(0, headerEnd) 正好取出所有请求头。
    std::string headerText = raw.substr(0, headerEnd);
    std::istringstream headerStream(headerText);

    std::string requestLine;
    std::getline(headerStream, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r') {
        // getline 以 '\n' 结束，Windows/HTTP 风格换行会留下 '\r'，需要去掉。
        requestLine.pop_back();
    }

    std::istringstream lineStream(requestLine);
    std::string version;
    // 请求行格式为：METHOD PATH HTTP/VERSION。当前只保存 method 和 path。
    lineStream >> request.method >> request.path >> version;
    if (request.method.empty() || request.path.empty()) {
        throw std::runtime_error("bad http request line");
    }

    // 去掉 query string。当前前端 API 使用 JSON body，不依赖 URL 参数。
    std::size_t queryPos = request.path.find('?');
    if (queryPos != std::string::npos) {
        request.path.resize(queryPos);
    }

    std::string line;
    // 后续每一行都是一个 header，格式一般为 Key: Value。
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            // 非法 header 行直接忽略，当前课程场景不做完整 HTTP 严格校验。
            continue;
        }

        // HTTP header 字段名大小写不敏感，统一转成小写保存。
        std::string key = lower_copy(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        request.headers[key] = value;
    }

    std::size_t contentLength = 0;
    auto lengthIt = request.headers.find("content-length");
    if (lengthIt != request.headers.end()) {
        // stoul 把字符串转为无符号整数；格式非法时会抛异常，由上层返回 400。
        contentLength = static_cast<std::size_t>(std::stoul(lengthIt->second));
    }

    // HTTP 头和 body 之间的分隔符 "\r\n\r\n" 长度为 4。
    std::size_t totalLength = headerEnd + 4 + contentLength;
    if (raw.size() < totalLength) {
        // POST body 还没收齐时不能消费 Buffer，否则下一次回调无法拼出完整请求。
        return false;
    }

    // body 从 headerEnd + 4 开始，长度由 Content-Length 指定。
    request.body = raw.substr(headerEnd + 4, contentLength);
    // 确认完整请求已经解析后，才从 muduo Buffer 中移除这些字节。
    buffer->retrieve(totalLength);
    return true;
}

/**
 * @brief 根据路径分发 HTTP 请求。
 */
std::string WebHttpServer::handle_request(const HttpRequest& request) const
{
    // API 路径直接走业务模块，其他路径都按静态文件处理。
    if (request.path == "/api/suggest") {
        return handle_api(request, true);
    }
    if (request.path == "/api/search") {
        return handle_api(request, false);
    }
    return handle_static_file(request);
}

/**
 * @brief 处理前端调用的 JSON API。
 */
std::string WebHttpServer::handle_api(const HttpRequest& request, bool keywordMode) const
{
    if (request.method != "POST") {
        return make_json_error(405, "api only supports POST");
    }

    // 前端以 JSON body 传参。body 为空时按空对象处理，便于返回 query is empty。
    // parse 失败会抛异常，最终由 on_message 返回 JSON 错误。
    nlohmann::json input = nlohmann::json::parse(request.body.empty() ? "{}" : request.body);
    // value("query", "") 表示字段不存在时使用默认空字符串。
    std::string query = input.value("query", "");
    if (query.empty()) {
        return make_json_error(400, "query is empty");
    }

    std::string body;
    if (keywordMode) {
        // /api/suggest 复用 KeywordRecommender。浏览器端只提交 query，推荐数量
        // 固定使用服务端配置，语言由 KeywordRecommender 自动判断。
        body = service_.suggest(query, "", keywordTopK_);
    } else {
        // /api/search 复用 WebSearcher。浏览器端不再传 topk，搜索数量由配置控制。
        body = service_.search(query, webTopK_);
    }

    return make_response(200, "OK", "application/json; charset=utf-8", body);
}

/**
 * @brief 返回 www 目录下的静态文件。
 */
std::string WebHttpServer::handle_static_file(const HttpRequest& request) const
{
    if (request.method != "GET" && request.method != "HEAD") {
        // 静态资源只接受 GET/HEAD，不接受 POST。
        return make_response(405, status_text(405), "text/plain; charset=utf-8", "method not allowed");
    }

    std::string contentType;
    std::string body = load_file(request.path, contentType);
    if (request.method == "HEAD") {
        // HEAD 只返回响应头，不返回实体内容。
        body.clear();
    }
    return make_response(200, "OK", contentType, body);
}

/**
 * @brief 组装完整 HTTP 响应字符串。
 */
std::string WebHttpServer::make_response(int statusCode,
                                         const std::string& statusText,
                                         const std::string& contentType,
                                         const std::string& body) const
{
    std::ostringstream response;
    // HTTP 响应由状态行、多个响应头、一个空行和响应体组成。
    // 每一行必须以 \r\n 结尾，这是 HTTP 协议的标准换行。
    response << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             // 明确短连接，浏览器收到响应后不再复用该 TCP 连接。
             << "Connection: close\r\n"
             << "\r\n"
             << body;
    // str() 将字符串流中累积的内容一次性取出。
    return response.str();
}

/**
 * @brief 生成 JSON 错误响应。
 */
std::string WebHttpServer::make_json_error(int statusCode, const std::string& message) const
{
    nlohmann::json error;
    error["error"] = "invalid request";
    error["message"] = message;
    // 错误响应也走 make_response，保证 Content-Length、Content-Type 等头部统一。
    return make_response(statusCode,
                         status_text(statusCode),
                         "application/json; charset=utf-8",
                         error.dump());
}

/**
 * @brief 按请求路径读取静态文件。
 */
std::string WebHttpServer::load_file(const std::string& requestPath, std::string& contentType) const
{
    // 访问根路径时默认返回前端首页。
    // 三目运算符让根路径映射逻辑保持在一行：条件成立取 index.html，否则取原路径。
    std::string relativePath = requestPath == "/" ? "/index.html" : requestPath;
    if (relativePath.find("..") != std::string::npos) {
        // 阻止通过 /../ 访问 wwwRoot_ 之外的文件。课程项目不引入复杂路径规范化。
        throw std::runtime_error("invalid path");
    }

    // relativePath 以 '/' 开头，substr(1) 去掉首斜杠后再拼到 wwwRoot_ 下。
    // std::filesystem::path 使用 / 运算符拼接路径，比手写字符串拼接更清晰。
    std::filesystem::path fullPath = std::filesystem::path(wwwRoot_) / relativePath.substr(1);
    std::ifstream ifs(fullPath, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("file not found: " + requestPath);
    }

    std::ostringstream buffer;
    // 直接把文件流缓冲区写入字符串流，适合当前较小的静态资源文件。
    // rdbuf() 返回文件流内部缓冲区，<< 可以把整个文件内容复制到字符串流。
    buffer << ifs.rdbuf();
    contentType = guess_content_type(fullPath.string());
    return buffer.str();
}
