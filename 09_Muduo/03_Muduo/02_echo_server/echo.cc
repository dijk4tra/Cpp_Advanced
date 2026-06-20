#include "echo.h"
#include <functional>
#include <muduo/base/Logging.h> // muduo 的日志模块
#include <muduo/base/Timestamp.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>

using std::placeholders::_1; // std::bind 的占位符
using std::placeholders::_2;
using std::placeholders::_3;

using namespace muduo;
using namespace muduo::net;

// 构造函数：初始化 TcpServer
EchoServer::EchoServer(EventLoop* loop, const InetAddress& listenAddr)
    : server_(loop, listenAddr, "EchoServer") // 传入loop,地址,服务器名称
{
    // 设置连接回调函数
    server_.setConnectionCallback(std::bind(&EchoServer::onConnection, this, _1));
    // 设置消息回调函数
    server_.setMessageCallback(std::bind(&EchoServer::onMessage, this, _1, _2, _3));
    // 设置 I/O 线程池大小为 4,即创建 4 个sub-Reator线程
    server_.setThreadNum(4);
}

// 启动服务器
void EchoServer::start()
{
    server_.start(); // TCPServer::start()开始监听
}

// 连接事件的回调
void EchoServer::onConnection(const TcpConnectionPtr& conn)
{
    // 输出连接信息: 客户端地址 -> 本地地址 的状态 (UP/DOWN)
    LOG_INFO << "EchoServer - " << conn->peerAddress().toIpPort() << " -> "
             << conn->localAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");
}

// 收到消息的回调
void EchoServer::onMessage(const TcpConnectionPtr& conn,
                           Buffer* buf,
                           Timestamp time)
{
    // 将缓冲区的所有数据作为一个字符串取出
    string msg(buf->retrieveAllAsString());
    // 日志记录: 连接名,字节数,接收时间
    LOG_INFO << conn->name() << " echo " << msg.size() << " bytes, "
             << "data received at " << time.toString();
    // 将数据原样发回客户端
    conn->send(msg);
}
