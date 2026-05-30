#pragma once

#include <muduo/net/TcpServer.h> // muduo 的 TCP 服务器类

// RFC 862 定义了 Echo 协议：服务器将客户端发送的数据原样返回
class EchoServer {
public:
    // 构造函数
    // loop: 事件循环（EventLoop），每个 TcpServer 需要绑定到一个 EventLoop
    // listenAddr: 监听的 IP 和端口地址
    EchoServer(muduo::net::EventLoop* loop,
        const muduo::net::InetAddress& listenAddr);

    // 启动服务器
    void start();

private:
    // 连接建立或关闭时的回调函数
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    // 收到消息时的回调函数
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
        muduo::net::Buffer* buf,
        muduo::Timestamp time);

    // muduo 的 TCP 服务器对象，负责网络 I/O 和事件分发
    muduo::net::TcpServer server_;
};
