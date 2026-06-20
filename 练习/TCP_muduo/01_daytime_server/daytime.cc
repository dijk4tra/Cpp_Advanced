#include "daytime.h"

#include <functional>
#include <muduo/base/Logging.h>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/EventLoop.h>
#include <sched.h>
#include <string>

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

using namespace muduo;
using namespace muduo::net;

// 构造函数实现
DaytimeServer::DaytimeServer(EventLoop* loop, const InetAddress& listenAddr)
    : server_(loop, listenAddr, "DaytimeServer")
{
    // 注册连接回调函数
    server_.setConnectionCallback(std::bind(&DaytimeServer::onConnection, this, _1));
    // 也可以在头文件中将onConnection函数改为static,就不需要再bind

    // 注册
    server_.setMessageCallback(std::bind(&DaytimeServer::onMessage, this, _1, _2, _3));

    // 设置 IO 线程池大小为 4，即创建 4 个 sub-Reactor 线程
    server_.setThreadNum(4);
}

// 启动服务器（内部调用 TcpServer 的 start 方法)
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

    // 如果连接已经建立 (UP状态)
    if (conn->connected()) {
        // 发送当前时间的字符串格式（例如 "2025-04-08 12:34:56.789\n"）
        conn->send(Timestamp::now().toFormattedString() + "\n");

        // 只关闭写端，服务器进入半连接状态，依然可以读取客户端的数据
        // conn->shutdown();
        // 主动关闭连接（Daytime 协议：发送完当前时间后服务器关闭连接）
        conn->forceClose();
    }
}

// 消息回调函数实现
void DaytimeServer::onMessage(const TcpConnectionPtr& conn,
                              Buffer* buf,
                              Timestamp time)
{
    // 读取缓冲区中的所有数据（无论客户端发来什么）
    string msg(buf->retrieveAllAsString());

    // 记录日志：丢弃了多少字节的数据，以及收到数据的时间
    LOG_INFO << conn->name() << " discards " << msg.size()
             << " bytes received at " << time.toString();
}
