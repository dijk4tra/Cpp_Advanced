#include "daytime.h"

#include <muduo/base/Logging.h> // 日志功能
#include <muduo/base/Timestamp.h> // 时间戳类
#include <muduo/net/Callbacks.h> // 回调函数类型定义
#include <muduo/net/EventLoop.h> // 事件循环

using namespace muduo;
using namespace muduo::net;

// 构造函数实现
DaytimeServer::DaytimeServer(EventLoop* loop, const InetAddress& listenAddr)
    : server_(loop, listenAddr, "DaytimeServer") // 初始化 TcpServer
{
    // 注册连接回调函数：当连接建立或关闭时，调用 onConnection
    server_.setConnectionCallback(std::bind(&DaytimeServer::onConnection, this, _1));

    // 注册消息回调函数：当收到数据时，调用 onMessage
    server_.setMessageCallback(std::bind(&DaytimeServer::onMessage, this, _1, _2, _3));
}

// 启动服务器（内部调用 TcpServer 的 start 方法）
void DaytimeServer::start()
{
    server_.start();
}

// 连接回调函数实现
void DaytimeServer::onConnection(const TcpConnectionPtr& conn)
{
    // 记录连接信息：客户端地址 -> 服务器地址，状态 UP 或 DOWN
    LOG_INFO << "DaytimeServer - " << conn->peerAddress().toIpPort() << " -> "
             << conn->localAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");

    // 如果连接已建立（UP 状态）
    if (conn->connected()) {
        // 发送当前时间的字符串格式（例如 "2025-04-08 12:34:56.789\n"）
        conn->send(Timestamp::now().toFormattedString() + "\n");

        // 只关闭写端，服务器进入半连接状态，依然可以读取客户端的数据
        // conn->shutdown();
        // 主动关闭连接（Daytime 协议：发送完时间后服务器关闭连接）
        conn->forceClose();
    }
}

// 消息回调函数实现
void DaytimeServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time)
{
    // 读取缓冲区中的所有数据（无论客户端发来什么）
    string msg(buf->retrieveAllAsString());

    // 记录日志：丢弃了多少字节的数据，以及收到数据的时间
    LOG_INFO << conn->name() << " discards " << msg.size()
             << " bytes received at " << time.toString();
}
