#include "../../include/online/ProtocolCodec.h"

#include <arpa/inet.h>
#include <cstdint>
#include <muduo/net/Buffer.h>

#include <cstring>
#include <netinet/in.h>
#include <stdexcept>

namespace
{
// TLV 固定头部长度：1 字节 type + 4 字节 length
constexpr std::size_t kHeaderSize = 5;

// 业务类型常量
// 100 仅用于错误响应，正常客户端请求只使用 1 和 2
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;
} // end of anonymous namespace

/**
 * @brief 从 muduo Buffer 中尝试拆出一条 TLV 消息。
 */
bool ProtocolCodec::try_decode(muduo::net::Buffer *buffer,
                               Request &request,
                               uint32_t maxMessageSize)
{
    if (buffer->readableBytes() < kHeaderSize) {
        return false;
    }

    const char* header = buffer->peek();
    uint8_t type = static_cast<uint8_t>(header[0]);
    if (type != kKeywordRequest && type != kWebRequest && type != kErrorResponse) {
        throw std::runtime_error("invalid message type");
    }

    uint32_t networkLength = 0;
    std::memcpy(&networkLength, header + 1, sizeof(networkLength));
    uint32_t length = ntohl(networkLength);
    if (length > maxMessageSize) {
        throw std::runtime_error("message is too large");
    }

    if (buffer->readableBytes() < kHeaderSize + length) {
        // 头部已到达但 body 还没有收齐，继续等待下一次 onMessage
        return false;
    }

    // 只有确认完整消息已经到达后，才从 Buffer 中取走数据
    // 先移动指针，跳过 5 字节的头部（前面已经利用 peek 提取并校验完毕）
    buffer->retrieve(kHeaderSize);
    request.type = type;
    request.value = buffer->retrieveAsString(length);
    return true;
}

/**
 * @brief 将业务 JSON 封装为 TLV 字节串。
 */
std::string ProtocolCodec::encode(uint8_t type, const std::string value)
{
    std::string packet;
    packet.reserve(kHeaderSize + value.size());

    packet.push_back(static_cast<char>(type));

    // 发送端必须使用网络序，接收端再通过 ntohl 还原，保证不同字节序机器互通
    uint32_t networkLength = htonl(static_cast<uint32_t>(value.size()));
    packet.append(reinterpret_cast<const char*>(&networkLength), sizeof(networkLength));
    packet.append(value);
    return packet;
}
