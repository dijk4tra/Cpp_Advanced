#pragma once

#include "LengthHeaderCodec.h"
#include <muduo/net/TcpClient.h>
#include <muduo/net/TcpConnection.h>

class ChatClient {
public:
    ChatClient(muduo::net::EventLoop* loop,
               const muduo::net::InetAddress& serverAddr);

    void connect(); // 连接服务器
    void disconnect(); // 断开连接
    void send(const std::string& message); // 发送消息

private:
    // 连接状态变化回调
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    // 收到完整消息回调
    void onEntireMessage(const muduo::net::TcpConnectionPtr&,
                         const std::string& message,
                         muduo::Timestamp);

private:
    muduo::net::TcpClient client_; // TCP 客户端
    muduo::net::EventLoop* loop_;  // 事件循环指针
    muduo::net::TcpConnectionPtr connection_; // 当前连接
    LengthHeaderCodec codec_; // 编解码器
};
