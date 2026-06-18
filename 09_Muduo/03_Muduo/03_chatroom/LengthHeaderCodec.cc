#include "LengthHeaderCodec.h"
#include <cstdint>
#include <endian.h> // 提供字节序转换函数

using namespace muduo;
using namespace muduo::net;

// 编码并发送消息
void LengthHeaderCodec::send(const TcpConnectionPtr &conn, const std::string &value)
{
    Buffer buf; // muduo 的缓冲区，自动管理内存
    uint32_t length_be = htobe32(value.size()); //主机字节序转网络字节序（大端）
    // 将长度头添加到缓冲区
    buf.append(&length_be, sizeof(length_be));
    // 将消息体添加到缓冲区
    buf.append(value.data(), value.size());
    // 通过 TCP 连接发送缓冲区中的数据
    conn->send(&buf);
}

// 解码接收到的数据（处理 TCP 粘包问题）
void LengthHeaderCodec::onMessage(const TcpConnectionPtr &conn,
                                  Buffer *buf,
                                  Timestamp receiveTime)
{
    // 循环处理, 因为缓冲区可能包含多条完整消息
    while (buf->readableBytes() >= 4) { // 至少要有 4 字节才能读取长度
        // 窥探(不取出)前 4 字节, 获取消息长度
        const void* data = buf->peek(); // peek() 只返回当前 readIndex 的地址
        // 网络字节序(大端)转主机字节序
        uint32_t length = be32toh(*static_cast<const uint32_t*>(data));

        // 检查消息是否完整: 总数据 = 4字节头 + length字节体
        if (buf->readableBytes() - 4 < length) {
            // 数据不完整, 等待下一次 TCP 数据到达
            break;
        }

        // 跳过长度头(4字节),即读指针变量readIndex+4
        buf->retrieve(4);
        // 提取消息体(用两个参数构造一个 std::string 对象)
        // std::string 中的构造函数: string(const char* s, size_t n);
        // 从 s 指向的内存中，拷贝 n 个字节，生成一个 string
        std::string value(buf->peek(), length);
        // 跳过已处理的消息体
        buf->retrieve(length);

        // 拿到一条完整的消息, 调用业务层注册的回调函数
        messageCallback_(conn, value, receiveTime);
    }
}

/*
    TcpServer 收到数据
        ↓
    调用 codec_.onMessage(...)
        ↓
    codec_ 解出完整消息
        ↓
    调用 messageCallback_(...)
        ↓
    实际执行 ChatServer::onEntireMessage(...)
*/
