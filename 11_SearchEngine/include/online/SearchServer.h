#pragma once

#include "KeywordRecommender.h"
#include "WebSearcher.h"

#include <muduo/net/TcpServer.h>

#include <cstdint>
#include <string>

/**
 * @brief 基于 muduo 的在线搜索服务器。
 *
 * SearchServer 只负责网络层和业务分发：收到 TLV 请求后解析 JSON，再根据
 * type 调用关键字推荐或网页搜索模块。
 * 两个业务模块的数据均在启动阶段加载，查询阶段只读，不在第二期引入缓存。
 *
 * 该服务监听配置项 server_port，专门提供 TLV 协议接口；
 * 浏览器测试使用 WebHttpServer 的 HTTP 端口，两者共享同一份业务对象。
 */
class SearchServer
{
public:
    /**
     * @brief 构造 TLV 搜索服务器并注册 muduo 回调。
     *
     * @param loop 主事件循环，由 main 创建并负责 loop()。
     * @param listenAddr TLV 服务监听地址。
     * @param recommender 关键字推荐模块引用，生命周期必须长于 SearchServer。
     * @param searcher 网页搜索模块引用，生命周期必须长于 SearchServer。
     * @param ioThreads muduo 工作线程数。
     * @param maxMessageSize TLV value 最大字节数。
     * @param keywordTopK 关键字推荐默认返回数量，请求 JSON 可用 topk 覆盖。
     * @param webTopK 网页搜索默认返回数量，请求 JSON 可用 topk 覆盖。
     */
    SearchServer(muduo::net::EventLoop* loop,
                 const muduo::net::InetAddress& listenAddr,
                 KeywordRecommender& recommender,
                 WebSearcher& searcher,
                 int ioThreads,
                 uint32_t maxMessageSize,
                 int keywordTopK,
                 int webTopK);

    /**
     * @brief 启动 TcpServer。
     */
    void start();

private:
    /**
     * @brief 连接建立或断开时打印日志。
     */
    void on_connection(const muduo::net::TcpConnectionPtr& conn);

    /**
     * @brief 处理 muduo 收到的字节流。
     *
     * 函数会循环调用 ProtocolCodec，直到 Buffer 中没有完整 TLV 消息为止。
     */
    void on_message(const muduo::net::TcpConnectionPtr& conn,
                    muduo::net::Buffer* buffer,
                    muduo::Timestamp receiveTime);

    /**
     * @brief 根据 TLV type 分发到关键字推荐或网页搜索。
     * @param type 请求类型。
     * @param value 请求 JSON 字符串。
     * @param responseType 输出参数，正常时等于请求 type，错误时改为 100。
     * @return 响应 JSON 字符串。
     * @throws nlohmann::json::exception 请求 JSON 格式非法时抛出。
     */
    std::string handle_request(uint8_t type, const std::string& value, uint8_t& responseType);

    /**
     * @brief 构造统一格式的错误 JSON。
     */
    std::string make_error(const std::string& message) const;

private:
    // muduo TCP 服务对象，负责监听、连接管理和线程池调度。
    muduo::net::TcpServer server_;

    // 业务模块引用。第二期查询阶段只读，不在 server 内部复制大索引。
    KeywordRecommender& recommender_;
    WebSearcher& searcher_;

    // 单条 TLV value 允许的最大字节数，防止异常客户端发送过大的消息。
    uint32_t maxMessageSize_;

    // 默认返回数量；TLV 请求 JSON 中提供 topk 时可以覆盖。
    int keywordTopK_;
    int webTopK_;
};
