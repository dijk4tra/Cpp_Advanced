#pragma once
#include <functional>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/TcpConnection.h>

// 协议格式: | Length (4字节网络字节序) | Value (Length字节) |
// 这种格式解决了TCP粘包问题：先读取4字节长度，再读取对应长度的消息体
class LengthHeaderCodec {
public:
    // 消息处理回调函数类型
    // 参数：连接指针、消息内容、接收时间戳
    using MessageCallback = std::function<void(const muduo::net::TcpConnectionPtr&,
        const std::string&,
        muduo::Timestamp)>;

    // 构造函数：保存消息处理回调
    explicit LengthHeaderCodec(const MessageCallback& cb)
        : messageCallback_(cb)
    {
    }

    // 编码：将消息打包成 [长度+内容] 格式发送
    void send(const muduo::net::TcpConnectionPtr& conn, const std::string& value);

    // 解码：从接收缓冲区解析出一条完整消息
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
        muduo::net::Buffer* buf,
        muduo::Timestamp receiveTime);

private:
    MessageCallback messageCallback_; // 解析完成后的回调函数
};
