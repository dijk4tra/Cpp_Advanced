#include "../../include/online/SearchServer.h"

#include "../../include/online/ProtocolCodec.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include <functional>
#include <nlohmann/json.hpp>

using namespace std::placeholders;

namespace
{
// TLV type 与 ProtocolCodec 保持一致。这里重复定义为匿名命名空间常量，
// 避免把协议细节暴露到公共头文件之外。
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;
}

/**
 * @brief 构造 TLV 搜索服务并设置 muduo 回调。
 */
SearchServer::SearchServer(muduo::net::EventLoop* loop,
                           const muduo::net::InetAddress& listenAddr,
                           KeywordRecommender& recommender,
                           WebSearcher& searcher,
                           int ioThreads,
                           uint32_t maxMessageSize,
                           int keywordTopK,
                           int webTopK)
    : server_(loop, listenAddr, "SearchServer")
    , recommender_(recommender)
    , searcher_(searcher)
    , maxMessageSize_(maxMessageSize)
    , keywordTopK_(keywordTopK)
    , webTopK_(webTopK)
{
    // std::bind 把成员函数绑定到当前对象。_1/_2/_3 是占位符，由 muduo 在回调
    // 发生时传入实际连接、缓冲区和时间戳。
    server_.setConnectionCallback(std::bind(&SearchServer::on_connection, this, _1));
    server_.setMessageCallback(std::bind(&SearchServer::on_message, this, _1, _2, _3));
    // 线程数来自配置文件。第二期未引入缓存，各线程只读共享业务数据。
    server_.setThreadNum(ioThreads);
}

/**
 * @brief 启动 muduo TcpServer。
 */
void SearchServer::start()
{
    server_.start();
}

/**
 * @brief 打印客户端连接状态。
 */
void SearchServer::on_connection(const muduo::net::TcpConnectionPtr& conn)
{
    if (conn->connected()) {
        LOG_INFO << "client connected: " << conn->peerAddress().toIpPort();
    } else {
        LOG_INFO << "client disconnected: " << conn->peerAddress().toIpPort();
    }
}

/**
 * @brief 处理 TLV 请求字节流。
 *
 * 一个 TCP 包里可能有多条 TLV，也可能只有半条 TLV。这里循环解包，直到当前
 * Buffer 不足以组成下一条完整消息。
 */
void SearchServer::on_message(const muduo::net::TcpConnectionPtr& conn,
                              muduo::net::Buffer* buffer,
                              muduo::Timestamp)
{
    try {
        Request request;
        while (ProtocolCodec::try_decode(buffer, request, maxMessageSize_)) {
            uint8_t responseType = request.type;
            std::string response = handle_request(request.type, request.value, responseType);
            conn->send(ProtocolCodec::encode(responseType, response));
        }
    } catch (const std::exception& ex) {
        // 协议错误、JSON 解析错误或业务处理异常统一封装为 type=100 的错误响应。
        conn->send(ProtocolCodec::encode(kErrorResponse, make_error(ex.what())));
    }
}

/**
 * @brief 解析请求 JSON 并分发给对应业务模块。
 */
std::string SearchServer::handle_request(uint8_t type, const std::string& value, uint8_t& responseType)
{
    nlohmann::json request = nlohmann::json::parse(value);
    std::string query = request.value("query", "");
    if (query.empty()) {
        responseType = kErrorResponse;
        return make_error("query is empty");
    }

    if (type == kKeywordRequest) {
        // 关键字推荐支持 lang，可显式指定 cn/en，也可留空由推荐模块自动判断。
        std::string lang = request.value("lang", "");
        int topK = request.value("topk", keywordTopK_);
        return recommender_.recommend_json(query, lang, topK);
    }

    if (type == kWebRequest) {
        // 网页搜索只需要查询词和返回数量。
        int topK = request.value("topk", webTopK_);
        return searcher_.search_json(query, topK);
    }

    responseType = kErrorResponse;
    return make_error("unsupported request type");
}

/**
 * @brief 生成统一错误 JSON。
 */
std::string SearchServer::make_error(const std::string& message) const
{
    nlohmann::json response;
    response["error"] = "invalid request";
    response["message"] = message;
    return response.dump();
}
