#pragma once

#include "LengthHeaderCodec.h"
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>
#include <set>

class ChatServer
{
public:
    // 构造函数
    ChatServer(muduo::net::EventLoop* loop,
               const muduo::net::InetAddress& listenAddr);

    void start();

private:
    // 处理连接建立或断开
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    // 处理完整的消息 (广播给其他所有的连接)
    void onEntireMessage(const muduo::net::TcpConnectionPtr& conn,
                         const std::string& message,
                         muduo::Timestamp receiveTime);

private:
    muduo::net::TcpServer server_; // TCP服务器
    LengthHeaderCodec codec_; // 编解码器
    std::set<muduo::net::TcpConnectionPtr> connections_;// 所有活跃连接的集合
};
