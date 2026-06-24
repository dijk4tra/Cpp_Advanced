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
 * type 调用关键字推荐或网页搜索模块。两个业务模块的数据均在启动阶段加载，
 * 查询阶段只读，不在第二期引入缓存。
 */
class SearchServer
{
public:
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
    void on_connection(const muduo::net::TcpConnectionPtr& conn);
    void on_message(const muduo::net::TcpConnectionPtr& conn,
                    muduo::net::Buffer* buffer,
                    muduo::Timestamp receiveTime);

    std::string handle_request(uint8_t type, const std::string& value, uint8_t& responseType);
    std::string make_error(const std::string& message) const;

private:
    muduo::net::TcpServer server_;
    KeywordRecommender& recommender_;
    WebSearcher& searcher_;
    uint32_t maxMessageSize_;
    int keywordTopK_;
    int webTopK_;
};
