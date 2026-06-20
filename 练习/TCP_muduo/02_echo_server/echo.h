#pragma once

#include <muduo/net/TcpServer.h>

class EchoServer
{
public:
    // 构造函数
    EchoServer(muduo::net::EventLoop* loop,
               const muduo::net::InetAddress& listenAddr);

    // 启动服务器
    void start();

private:
    // 连接建立或断开时的回调函数
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    // 收到消息时的回调函数
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
                   muduo::net::Buffer* buf,
                   muduo::Timestamp time);

    // muduo 的 TCP 服务器对象，负责网络 I/O 和事件分发
    muduo::net::TcpServer server_;
};
