#include "../../include/online/SearchServer.h"

#include "../../include/online/ProtocolCodec.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include <functional>
#include <nlohmann/json.hpp>

// 让 std::bind 中可以直接使用 _1、_2、_3。
// 这些占位符表示 muduo 调用回调时传进来的第 1、第 2、第 3 个实参。
using namespace std::placeholders;

namespace
{
// TLV type 与 ProtocolCodec 保持一致。这里重复定义为匿名命名空间常量，
// 避免把协议细节暴露到公共头文件之外。
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;
// constexpr 表示编译期常量，协议类型在运行时不会被修改。
}

/**
 * @brief 构造 TLV 搜索服务并设置 muduo 回调。
 */
SearchServer::SearchServer(muduo::net::EventLoop* loop,
                           const muduo::net::InetAddress& listenAddr,
                           CachedSearchService& service,
                           int ioThreads,
                           uint32_t maxMessageSize,
                           int keywordTopK,
                           int webTopK)
    // 成员初始化列表会在进入构造函数函数体之前初始化成员。
    // server_ 没有默认构造后再设置监听地址的过程，因此必须在这里直接构造。
    : server_(loop, listenAddr, "SearchServer")
    // service_ 是引用成员，必须在初始化列表中绑定。
    , service_(service)
    , maxMessageSize_(maxMessageSize)
    , keywordTopK_(keywordTopK)
    , webTopK_(webTopK)
{
    // std::bind 把成员函数绑定到当前对象。_1/_2/_3 是占位符，由 muduo 在回调
    // 发生时传入实际连接、缓冲区和时间戳。
    // 成员函数自带一个隐藏的 this 指针，因此绑定时必须把 this 也传进去。
    server_.setConnectionCallback(std::bind(&SearchServer::on_connection, this, _1));
    server_.setMessageCallback(std::bind(&SearchServer::on_message, this, _1, _2, _3));
    // 线程数来自配置文件。第三期缓存对象内部负责并发控制，网络层只共享业务服务引用。
    server_.setThreadNum(ioThreads);
}

/**
 * @brief 启动 muduo TcpServer。
 */
void SearchServer::start()
{
    // start() 只是开始监听并准备接受连接，真正的事件处理发生在 main 中的 loop.loop()。
    server_.start();
}

/**
 * @brief 打印客户端连接状态。
 */
void SearchServer::on_connection(const muduo::net::TcpConnectionPtr& conn)
{
    // TcpConnectionPtr 是 muduo 定义的智能指针，连接对象由 muduo 管理生命周期。
    if (conn->connected()) {
        LOG_DEBUG << "client connected: " << conn->peerAddress().toIpPort();
    } else {
        LOG_DEBUG << "client disconnected: " << conn->peerAddress().toIpPort();
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
        // while 循环用于处理粘包：一次 on_message 可能已经收到了多条完整 TLV。
        while (ProtocolCodec::try_decode(buffer, request, maxMessageSize_)) {
            // 默认响应类型与请求类型相同；如果业务层发现错误，会通过引用参数改成 100。
            uint8_t responseType = request.type;
            std::string response = handle_request(request.type, request.value, responseType);
            // 业务层只返回 JSON 字符串，发送前仍需重新封装成 TLV。
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
    // parse 会把请求体字符串解析成 JSON 对象；格式错误时抛异常，由 on_message 捕获。
    nlohmann::json request = nlohmann::json::parse(value);
    // value("query", "") 表示读取 query 字段；字段不存在时使用默认空字符串。
    std::string query = request.value("query", "");
    if (query.empty()) {
        responseType = kErrorResponse;
        return make_error("query is empty");
    }

    if (type == kKeywordRequest) {
        // 关键字推荐支持 lang，可显式指定 cn/en，也可留空由推荐模块自动判断。
        std::string lang = request.value("lang", "");
        // TLV 客户端可以在请求 JSON 中传 topk；未传时使用服务端配置默认值。
        int topK = request.value("topk", keywordTopK_);
        return service_.suggest(query, lang, topK);
    }

    if (type == kWebRequest) {
        // TLV 客户端可以在请求 JSON 中传 topk；未传时使用服务端配置默认值。
        int topK = request.value("topk", webTopK_);
        return service_.search(query, topK);
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
    // 错误响应保持统一字段，客户端只要判断是否存在 error 即可识别失败。
    response["error"] = "invalid request";
    response["message"] = message;
    return response.dump();
}
