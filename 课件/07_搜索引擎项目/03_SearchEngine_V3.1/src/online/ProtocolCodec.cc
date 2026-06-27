#include "../../include/online/ProtocolCodec.h"

#include <arpa/inet.h>
#include <muduo/net/Buffer.h>

#include <cstring>
#include <stdexcept>

namespace
{
// TLV 固定头部长度：1 字节 type + 4 字节 length。
constexpr std::size_t kHeaderSize = 5;
// kHeaderSize 中的 k 表明它是一个编译期常量(constexpr)，
// 它的值在程序运行期间绝对不会、也不能被修改

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
    // TCP 是字节流，一次 onMessage 可能只收到半个请求。
    // 因此先判断固定头部是否完整，不完整时保留缓冲区内容等待下一次回调。
    if (buffer->readableBytes() < kHeaderSize) {
        return false;
    }

    // 提取消息头部的第 1 个字节（header[0]），header[0] 即为 TLV 的第一个字节（Type）
    // 使用 peek() 仅观察缓冲区首地址，暂不移动读指针(Read Index)
    const char* header = buffer->peek();
    // 显式转换为无符号整型。防止部分平台默认 char 为有符号型时，
    // 大于 127 的协议类型值（如未来扩展的Type）被误识别为负数，导致比较失效。
    uint8_t type = static_cast<uint8_t>(header[0]);
    if (type != kKeywordRequest && type != kWebRequest && type != kErrorResponse) {
        throw std::runtime_error("invalid message type");
    }

    // 协议中 Length 位于 header + 1 的位置。因为 Type 占了 1 字节，导致 Length 的起始地址是一个奇数地址。
    // 如果直接使用指针强转 `*reinterpret_cast<const uint32_t*>(header + 1)`，在 ARM/MIPS 等 RISC 架构上
    // 会因为访问未对齐内存而触发硬件异常（Bus Error 崩溃），在 x86 架构上也会引发严重的性能损耗。
    //
    // 解决方案：声明一个在栈上严格对齐的局部变量 networkLength，通过 std::memcpy 逐字节将数据
    // 安全地复制过来，利用编译器对 memcpy 的底层优化，完美绕过未对齐内存访问的硬件限制，具备极强的跨平台可移植性。
    uint32_t networkLength = 0;
    std::memcpy(&networkLength, header + 1, sizeof(networkLength));
    // TCP/IP 协议规定网络传输一律使用大端字节序（Big-Endian），而目前主流服务器 CPU
    // 均为小端字节序（Little-Endian）。必须调用 ntohl() 将其还原为主机能正确识别的十进制整数。
    uint32_t length = ntohl(networkLength);

    // 防御性检查: 如果他人故意伪造极其巨大的 length（例如 0xFFFFFFFF），
    // 后续代码可能会盲目等待或分配高达几个GB的内存，从而导致服务器内存耗尽（OOM）崩溃。
    if (length > maxMessageSize) {
        throw std::runtime_error("message is too large");
    }

    if (buffer->readableBytes() < kHeaderSize + length) {
        // 头部已到达但 body 还没有收齐，继续等待下一次 onMessage。
        return false;
    }

    // 确认完整消息已经到达，正式从 Buffer 中取走数据。
    // 先移动指针，跳过 5 字节的头部（前面已经利用 peek 提取并校验完毕）
    buffer->retrieve(kHeaderSize);
    request.type = type;
    // retrieveAsString 会从当前读指针位置取出指定长度的字节构建成 std::string，
    // 并在 muduo::net::Buffer 内部自动向后移动读指针 `length` 字节。
    request.value = buffer->retrieveAsString(length);
    return true;
}

/**
 * @brief 将业务 JSON 封装为 TLV 字节串。
 */
std::string ProtocolCodec::encode(uint8_t type, const std::string& value)
{
    std::string packet;
    // 预先一次性分配足够的内存空间（5字节固定头部 + Body实际长度）
    // 避免 std::string 在构建过程中因容量不足频繁触发堆内存重新分配与数据拷贝，
    // 从而显著降低高并发场景下的 CPU 额外开销。
    packet.reserve(kHeaderSize + value.size());

    // 1. 写入 1 字节的 Type（业务类型）字段
    packet.push_back(static_cast<char>(type));

    // 2. 写入 4 字节的 Length（数据体长度）字段
    // 发送端必须使用网络序，接收端再通过 ntohl 还原，保证不同字节序机器互通。
    uint32_t networkLength = htonl(static_cast<uint32_t>(value.size()));
    // 通过指针类型转换，将这 4 字节的二进制内存数据直接追加至字符串缓冲区
    packet.append(reinterpret_cast<const char*>(&networkLength), sizeof(networkLength));

    // 3. 追加实际的 Value（业务数据体）
    packet.append(value);

    return packet;
}
