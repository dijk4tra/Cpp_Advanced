#pragma once

#include "LengthHeaderCodec.h"
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/TcpServer.h>
#include <set>

// 聊天服务器主类
class ChatServer {
public:
    ChatServer(muduo::net::EventLoop* loop, const muduo::net::InetAddress& listenAddr);
    void start();

private:
    // 处理连接建立和断开事件
    void onConnection(const muduo::net::TcpConnectionPtr& conn);
    // 处理完整的消息（广播给所有其它连接）
    void onEntireMessage(const muduo::net::TcpConnectionPtr& conn,
        const std::string& message,
        muduo::Timestamp receiveTime);

    muduo::net::TcpServer server_; // TCP服务器
    LengthHeaderCodec codec_; // 编解码器
    std::set<muduo::net::TcpConnectionPtr> connections_; // 所有活跃连接的集合
};
