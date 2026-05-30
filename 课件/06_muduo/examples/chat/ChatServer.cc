#include "ChatServer.h"
#include <muduo/base/Logging.h>
#include <muduo/base/Timestamp.h>

using namespace muduo;
using namespace muduo::net;

ChatServer::ChatServer(EventLoop* loop, const InetAddress& listenAddr)
    : server_(loop, listenAddr, "ChatServer")
    , codec_(std::bind(&ChatServer::onEntireMessage, this, _1, _2, _3))
{
    // 注册连接回调：当有新客户端连接或断开时调用
    server_.setConnectionCallback(
        std::bind(&ChatServer::onConnection, this, _1));

    // 注册消息回调：使用编解码器处理消息分包
    // 解码器解析出完整消息后，会调用 onEntireMessage
    server_.setMessageCallback(
        std::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));

    // 设置I/O线程数为4，提高并发处理能力
    server_.setThreadNum(4);
}

void ChatServer::start()
{
    server_.start();
    LOG_INFO << "ChatServer started on port " << server_.ipPort();
}

// 处理客户端连接和断开事件
void ChatServer::onConnection(const TcpConnectionPtr& conn)
{
    if (conn->connected()) {
        // 新客户端连接，加入连接集合
        connections_.insert(conn);
        LOG_INFO << conn->peerAddress().toIpPort()
                 << " -> " << conn->localAddress().toIpPort()
                 << " is UP, total connections: " << connections_.size();

        // 发送欢迎消息，告知当前在线人数
        string welcome = "Welcome to chat room! Current online users: "
            + std::to_string(connections_.size());
        codec_.send(conn, welcome);
    } else {
        // 客户端断开连接，从集合中移除
        connections_.erase(conn);
        LOG_INFO << conn->peerAddress().toIpPort()
                 << " is DOWN, remaining connections: " << connections_.size();
    }
}

// 处理完整的字符串消息（广播给所有其他客户端）
void ChatServer::onEntireMessage(const TcpConnectionPtr& conn,
    const string& message,
    Timestamp receiveTime)
{
    // 格式化消息：添加时间戳前缀
    string formattedMsg = receiveTime.toFormattedString() + ": " + message;
    LOG_INFO << "Broadcasting: " << formattedMsg;

    // 遍历所有连接，将消息广播给除发送者外的每个客户端
    for (const auto& c : connections_) {
        if (c != conn) { // 不发送给自己
            codec_.send(c, formattedMsg);
        }
    }
}
