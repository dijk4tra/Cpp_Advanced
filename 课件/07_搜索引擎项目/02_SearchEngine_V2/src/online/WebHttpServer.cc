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

using namespace std::placeholders;

namespace
{
/**
 * @brief 返回字符串的小写副本。
 *
 * HTTP 头字段名大小写不敏感，解析时统一转小写，后续查找 header 更简单。
 */
std::string lower_copy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
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
    auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
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
                             KeywordRecommender& recommender,
                             WebSearcher& searcher,
                             const std::string& wwwRoot,
                             int httpThreads,
                             int keywordTopK,
                             int webTopK)
    : server_(loop, listenAddr, "WebHttpServer")
    , recommender_(recommender)
    , searcher_(searcher)
    , wwwRoot_(wwwRoot)
    , keywordTopK_(keywordTopK)
    , webTopK_(webTopK)
{
    // 与 TLV 服务一样，HTTP 服务也基于 muduo TcpServer；区别只是上层协议解析。
    server_.setConnectionCallback(std::bind(&WebHttpServer::on_connection, this, _1));
    server_.setMessageCallback(std::bind(&WebHttpServer::on_message, this, _1, _2, _3));
    server_.setThreadNum(httpThreads);
}

/**
 * @brief 启动 HTTP TcpServer。
 */
void WebHttpServer::start()
{
    server_.start();
}

/**
 * @brief 打印 HTTP 客户端连接状态。
 */
void WebHttpServer::on_connection(const muduo::net::TcpConnectionPtr& conn)
{
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
        conn->send(handle_request(request));
        conn->shutdown();
    } catch (const std::exception& ex) {
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
    std::string raw(buffer->peek(), buffer->readableBytes());
    std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }

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
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
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
        contentLength = static_cast<std::size_t>(std::stoul(lengthIt->second));
    }

    std::size_t totalLength = headerEnd + 4 + contentLength;
    if (raw.size() < totalLength) {
        // POST body 还没收齐时不能消费 Buffer，否则下一次回调无法拼出完整请求。
        return false;
    }

    request.body = raw.substr(headerEnd + 4, contentLength);
    buffer->retrieve(totalLength);
    return true;
}

/**
 * @brief 根据路径分发 HTTP 请求。
 */
std::string WebHttpServer::handle_request(const HttpRequest& request) const
{
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
    nlohmann::json input = nlohmann::json::parse(request.body.empty() ? "{}" : request.body);
    std::string query = input.value("query", "");
    if (query.empty()) {
        return make_json_error(400, "query is empty");
    }

    std::string body;
    if (keywordMode) {
        // /api/suggest 复用 KeywordRecommender，返回格式与 TLV 关键字推荐一致。
        int topK = input.value("topk", keywordTopK_);
        std::string lang = input.value("lang", "");
        body = recommender_.recommend_json(query, lang, topK);
    } else {
        // /api/search 复用 WebSearcher，返回格式与 TLV 网页搜索一致。
        int topK = input.value("topk", webTopK_);
        body = searcher_.search_json(query, topK);
    }

    return make_response(200, "OK", "application/json; charset=utf-8", body);
}

/**
 * @brief 返回 www 目录下的静态文件。
 */
std::string WebHttpServer::handle_static_file(const HttpRequest& request) const
{
    if (request.method != "GET" && request.method != "HEAD") {
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
    response << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             // 明确短连接，浏览器收到响应后不再复用该 TCP 连接。
             << "Connection: close\r\n"
             << "\r\n"
             << body;
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
    std::string relativePath = requestPath == "/" ? "/index.html" : requestPath;
    if (relativePath.find("..") != std::string::npos) {
        // 阻止通过 /../ 访问 wwwRoot_ 之外的文件。课程项目不引入复杂路径规范化。
        throw std::runtime_error("invalid path");
    }

    // relativePath 以 '/' 开头，substr(1) 去掉首斜杠后再拼到 wwwRoot_ 下。
    std::filesystem::path fullPath = std::filesystem::path(wwwRoot_) / relativePath.substr(1);
    std::ifstream ifs(fullPath, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("file not found: " + requestPath);
    }

    std::ostringstream buffer;
    // 直接把文件流缓冲区写入字符串流，适合当前较小的静态资源文件。
    buffer << ifs.rdbuf();
    contentType = guess_content_type(fullPath.string());
    return buffer.str();
}
