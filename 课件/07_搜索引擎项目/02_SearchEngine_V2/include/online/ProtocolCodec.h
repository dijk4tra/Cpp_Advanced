#pragma once

#include <cstdint>
#include <string>

namespace muduo
{
namespace net
{
class Buffer;
}
}

/**
 * @brief 一条完成解包后的 TLV 请求消息。
 *
 * type 表示业务类型，value 保存 JSON 字符串。协议头中的 length 只用于网络
 * 传输阶段，解析完成后不再单独保存。
 */
struct Request
{
    // 业务类型：1 表示关键字推荐，2 表示网页搜索。
    uint8_t type = 0;

    // TLV 中的 value 部分，约定保存 JSON 字符串。
    std::string value;
};

/**
 * @brief TLV 协议编解码工具。
 *
 * 协议格式固定为：1 字节 type + 4 字节网络序 length + length 字节 value。
 * 该类不保存状态，只负责从 muduo Buffer 中按消息边界读取数据，以及把 JSON
 * 响应封装回同样的 TLV 字节串。
 *
 * TCP 本身没有消息边界，try_decode() 的返回值用于告诉上层当前 Buffer 中是否
 * 已经凑齐一条完整消息；数据不足时不会消耗 Buffer。
 */
class ProtocolCodec
{
public:
    /**
     * @brief 尝试从 buffer 中解析一条完整请求。
     *
     * @param buffer muduo 接收缓冲区。
     * @param request 输出参数，解析成功时写入完整请求。
     * @param maxMessageSize 允许的最大 value 字节数。
     * @return 解析到完整消息时返回 true；数据不足时返回 false。
     * @throws std::runtime_error length 超出限制或消息类型非法时抛出。
     */
    static bool try_decode(muduo::net::Buffer* buffer,
                           Request& request,
                           uint32_t maxMessageSize);

    /**
     * @brief 将业务响应 JSON 封装为 TLV 字节串。
     * @param type 响应类型，通常与请求类型相同，错误响应使用 100。
     * @param value JSON 字符串。
     * @return 可直接发送给 TcpConnection::send() 的完整字节串。
     */
    static std::string encode(uint8_t type, const std::string& value);
};
