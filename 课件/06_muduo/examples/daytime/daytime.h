#pragma once

#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

// RFC 867
// 功能：当客户端连接时，服务器返回当前的日期时间字符串，然后关闭连接。
// 如果客户端发送数据，服务器会接收并丢弃（忽略）。
class DaytimeServer {
public:
    // 构造函数
    // @param loop: 事件循环（EventLoop），用于处理网络事件
    // @param listenAddr: 服务器监听的地址和端口
    DaytimeServer(muduo::net::EventLoop* loop,
        const muduo::net::InetAddress& listenAddr);

    // 启动服务器（开始监听）
    void start();

private:
    // 连接回调函数：当新连接建立或断开时被调用
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    // 消息回调函数：当收到客户端数据时被调用
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
        muduo::net::Buffer* buf,
        muduo::Timestamp time);

private:
    muduo::net::TcpServer server_; // TCP 服务器对象，管理监听和连接
};
