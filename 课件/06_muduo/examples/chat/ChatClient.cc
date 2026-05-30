#include "ChatClient.h"
#include <iostream>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

using namespace std;
using namespace muduo;
using namespace muduo::net;

ChatClient::ChatClient(EventLoop* loop, const InetAddress& serverAddr)
    : client_(loop, serverAddr, "ChatClient") // 初始化TCP客户端
    , loop_(loop)
    , codec_(std::bind(&ChatClient::onEntireMessage, this, _1, _2, _3))
{
    // 注册连接回调：当与服务器的连接建立或断开时调用
    client_.setConnectionCallback(
        std::bind(&ChatClient::onConnection, this, _1));
    // 注册消息回调：使用编解码器处理接收到的数据
    client_.setMessageCallback(
        std::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));
}

void ChatClient::connect()
{
    client_.connect(); // 发起连接到服务器
}

void ChatClient::disconnect()
{
    client_.disconnect(); // 断开与服务器的连接
}

void ChatClient::send(const string& message)
{
    if (connection_) {
        codec_.send(connection_, message); // 通过编解码器发送消息
    }
}

void ChatClient::onConnection(const TcpConnectionPtr& conn)
{
    if (conn->connected()) {
        // 连接建立成功，保存连接指针
        connection_ = conn;
        LOG_INFO << "Connected to server: "
                 << conn->peerAddress().toIpPort();
        cout << "=== Connected to chat server ===" << endl;
        cout << "Type your message and press Enter to send." << endl;
        cout << "Type '/quit' to exit." << endl;
        cout << "================================" << endl;
    } else {
        // 连接断开，重置连接指针
        connection_.reset();
        LOG_INFO << "Disconnected from server";
        cout << "=== Disconnected from server ===" << endl;
        // 退出事件循环，程序结束
        loop_->quit();
    }
}

// 收到服务器广播的消息时的回调
void ChatClient::onEntireMessage(const TcpConnectionPtr& conn,
    const string& message,
    Timestamp receiveTime)
{
    // 直接显示收到的消息（服务器已经添加了时间戳）
    cout << message << endl;
}
