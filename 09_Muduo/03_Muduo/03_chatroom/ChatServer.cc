#include "ChatServer.h"
#include <muduo/base/Logging.h>
#include <muduo/base/Timestamp.h>
#include <string>
#include <sys/sysinfo.h>

using namespace muduo;
using namespace muduo::net;

ChatServer::ChatServer(EventLoop* loop, const InetAddress& listenAddr)
    : server_(loop, listenAddr, "ChatServer")
    , codec_(std::bind(&ChatServer::onEntireMessage, this, _1, _2, _3))
    // onEntireMessage 是 ChatServer 的成员函数
{
    // 注册连接回调: 当有新客户端连接建立或者断开时调用
    server_.setConnectionCallback(
        std::bind(&ChatServer::onConnection, this, _1));
        // 成员函数，必须绑定对象
        // 当 muduo 检测到客户端连接/断开时，
        // 调用当前 ChatServer 对象的 onConnection(conn)

    // 注册消息回调: 使用编解码器处理消息分包
    // 解码器解析出完整消息后, 会调用 onEntireMessage
    server_.setMessageCallback(
        std::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));
        // 这里调用的是 LengthHeaderCodec 的成员函数 onMessage
        // 所以需要绑定 LengthHeaderCodec 类的对象 codec_


    // 设置 I/O 线程数为 CPU 逻辑核心数,提高并发处理能力
    int procs_num = get_nprocs();
    server_.setThreadNum(procs_num);
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
        // 新客户端连接,加入连接集合
        // peerAddress指的是连接到当前程序的“远程（对端）主机”的 IP 地址
        connections_.insert(conn);
        LOG_INFO << conn->peerAddress().toIpPort()
                 << " -> " << conn->localAddress().toIpPort()
                 << " is UP, total connection: " << connections_.size();

        // 发送欢迎消息, 告知当前在线人数
        string welcome = "Welcome to chat room! Current online users: "
            + std::to_string(connections_.size());
        codec_.send(conn, welcome);
    } else {
        // 客户端断开连接,从集合中移除
        connections_.erase(conn);
        LOG_INFO << conn->peerAddress().toIpPort()
                 << " is DOWN, remaining connections: " << connections_.size();
    }
}

// 处理完整的字符串消息 (广播给所有其他客户端)
void ChatServer::onEntireMessage(const TcpConnectionPtr& conn,
                                 const string& message,
                                 Timestamp receiveTime)
{
    // 格式化消息: 添加时间戳前缀
    string formattedMsg = receiveTime.toFormattedString() + ": " + message;
    LOG_INFO << "Broadcasting: " << formattedMsg;

    // 遍历所有连接,将消息广播发给除发送者外的每个客户端
    for (const auto& c : connections_) {
        if (c != conn) { // 不发给自己
            codec_.send(c, formattedMsg);
        }
    }
}
