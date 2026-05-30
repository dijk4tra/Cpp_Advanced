#include "LengthHeaderCodec.h"
#include <endian.h> // 提供字节序转换函数

using namespace muduo;
using namespace muduo::net;

// 编码并发送消息
void LengthHeaderCodec::send(const TcpConnectionPtr& conn, const std::string& value)
{
    Buffer buf; // muduo的缓冲区，自动管理内存
    uint32_t length_be = htobe32(value.size()); // 主机字节序转网络字节序（大端）
    // 将长度头添加到缓冲区
    buf.append(&length_be, sizeof(length_be));
    // 将消息体添加到缓冲区
    buf.append(value.data(), value.size());
    // 通过TCP连接发送缓冲区中的数据
    conn->send(&buf);
}

// 解码接收到的数据（处理TCP粘包问题）
void LengthHeaderCodec::onMessage(const TcpConnectionPtr& conn,
    Buffer* buf,
    Timestamp receiveTime)
{
    // 循环处理，因为缓冲区可能包含多条完整消息
    while (buf->readableBytes() >= 4) { // 至少要有4字节才能读取长度
        // 窥探（不取出）前4字节，获取消息长度
        const void* data = buf->peek();
        uint32_t length = be32toh(*static_cast<const uint32_t*>(data)); // 网络字节序转主机

        // 检查消息是否完整：总数据量 = 4字节头 + length字节体
        if (buf->readableBytes() - 4 < length) {
            // 数据不完整，等待下一次TCP数据到达
            break;
        }

        // 跳过长度头（4字节）
        buf->retrieve(4);
        // 提取消息体
        std::string value(buf->peek(), length);
        // 跳过已处理的消息体
        buf->retrieve(length);

        // 好莱坞原则：Don't call me, I'll call you back.
        // 拿到一条完整的消息，调用业务层注册的回调函数
        messageCallback_(conn, value, receiveTime);
    }
}
