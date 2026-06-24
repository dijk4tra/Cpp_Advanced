#include "../../include/online/ProtocolCodec.h"

#include <arpa/inet.h>
#include <muduo/net/Buffer.h>

#include <cstring>
#include <stdexcept>

namespace
{
// TLV 固定头部长度：1 字节 type + 4 字节 length。
constexpr std::size_t kHeaderSize = 5;

// 业务类型常量。100 仅用于错误响应，正常客户端请求只使用 1 和 2。
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;
}

/**
 * @brief 从 muduo Buffer 中尝试拆出一条 TLV 消息。
 */
bool ProtocolCodec::try_decode(muduo::net::Buffer* buffer,
                               Request& request,
                               uint32_t maxMessageSize)
{
    // TCP 是字节流，一次 onMessage 可能只收到半个请求。因此先判断固定头部
    // 是否完整，不完整时保留缓冲区内容等待下一次回调。
    if (buffer->readableBytes() < kHeaderSize) {
        return false;
    }

    const char* header = buffer->peek();
    uint8_t type = static_cast<uint8_t>(header[0]);
    if (type != kKeywordRequest && type != kWebRequest && type != kErrorResponse) {
        throw std::runtime_error("invalid message type");
    }

    // length 在网络中使用大端字节序。memcpy 避免直接把未对齐地址转换为
    // uint32_t*，这样在不同 CPU 架构上都更稳妥。
    uint32_t networkLength = 0;
    std::memcpy(&networkLength, header + 1, sizeof(networkLength));
    uint32_t length = ntohl(networkLength);
    if (length > maxMessageSize) {
        throw std::runtime_error("message is too large");
    }

    if (buffer->readableBytes() < kHeaderSize + length) {
        // 头部已到达但 body 还没有收齐，继续等待下一次 onMessage。
        return false;
    }

    // 只有确认完整消息已经到达后，才从 Buffer 中取走数据。
    buffer->retrieve(kHeaderSize);
    request.type = type;
    request.value = buffer->retrieveAsString(length);
    return true;
}

/**
 * @brief 将业务 JSON 封装为 TLV 字节串。
 */
std::string ProtocolCodec::encode(uint8_t type, const std::string& value)
{
    std::string packet;
    packet.reserve(kHeaderSize + value.size());

    packet.push_back(static_cast<char>(type));

    // 发送端必须使用网络序，接收端再通过 ntohl 还原，保证不同字节序机器互通。
    uint32_t networkLength = htonl(static_cast<uint32_t>(value.size()));
    packet.append(reinterpret_cast<const char*>(&networkLength), sizeof(networkLength));
    packet.append(value);
    return packet;
}
