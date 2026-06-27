#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
// 与服务端 SearchServer.cc 中的协议类型保持一致。
// type=1 走关键字推荐，type=2 走网页搜索，type=100 是服务端错误响应。
constexpr uint8_t kKeywordRequest = 1;
constexpr uint8_t kWebRequest = 2;
constexpr uint8_t kErrorResponse = 100;

// 命令行参数解析后的统一配置。这个客户端刻意不依赖项目内部源码，
// 方便单独用 g++ 编译后测试已经运行起来的 TLV 服务。
struct Options
{
    std::string host = "127.0.0.1";
    std::string port = "8888";
    uint8_t type = kWebRequest;
    std::string query;
    std::string lang;
    bool hasTopK = false;
    int topK = 0;
    std::string rawJson;
};

// 把协议 type 转成人可读名称，只用于打印调试信息。
std::string type_name(uint8_t type)
{
    switch (type) {
    case kKeywordRequest:
        return "keyword";
    case kWebRequest:
        return "web";
    case kErrorResponse:
        return "error";
    default:
        return "unknown";
    }
}

// std::stoi 允许 "12abc" 这种部分转换成功的输入，所以这里额外检查 pos。
int parse_int(const std::string& text, const std::string& name)
{
    std::size_t pos = 0;
    int value = std::stoi(text, &pos);
    if (pos != text.size()) {
        throw std::runtime_error("invalid " + name + ": " + text);
    }
    return value;
}

// 支持数字协议类型，也支持更适合命令行输入的别名。
uint8_t parse_type(const std::string& text)
{
    if (text == "keyword" || text == "kw" || text == "1") {
        return kKeywordRequest;
    }
    if (text == "web" || text == "search" || text == "2") {
        return kWebRequest;
    }
    throw std::runtime_error("invalid request type: " + text);
}

// 输出最小但完整的使用说明。出错时也会打印它，方便直接修正命令。
void print_usage(const char* program)
{
    std::cerr
        << "Usage:\n"
        << "  " << program << " --type keyword --query TEXT [--lang cn|en] [--topk N]\n"
        << "  " << program << " --type web --query TEXT [--topk N]\n"
        << "  " << program << " --type 1 --raw-json JSON\n\n"
        << "Options:\n"
        << "  --host HOST       server host, default 127.0.0.1\n"
        << "  --port PORT       TLV server port, default 8888\n"
        << "  --type TYPE       keyword|web|1|2\n"
        << "  --query TEXT      query text used to build JSON\n"
        << "  --lang LANG       keyword language hint: cn or en\n"
        << "  --topk N          optional TLV topk override\n"
        << "  --raw-json JSON   send this JSON string as TLV value directly\n"
        << "  --help            show this help\n";
}

// 手写一个轻量参数解析器，避免为了测试工具额外引入命令行解析库。
Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // 所有带值参数都通过这个 lambda 取下一个 argv，并统一处理缺值错误。
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--host") {
            options.host = need_value(arg);
        } else if (arg == "--port") {
            options.port = need_value(arg);
        } else if (arg == "--type") {
            options.type = parse_type(need_value(arg));
        } else if (arg == "--query") {
            options.query = need_value(arg);
        } else if (arg == "--lang") {
            options.lang = need_value(arg);
        } else if (arg == "--topk") {
            options.topK = parse_int(need_value(arg), "topk");
            options.hasTopK = true;
        } else if (arg == "--raw-json") {
            options.rawJson = need_value(arg);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (options.rawJson.empty() && options.query.empty()) {
        throw std::runtime_error("--query is required unless --raw-json is used");
    }
    return options;
}

// 根据命令行参数构造 TLV value 部分的 JSON。
// 如果传了 --raw-json，则完全尊重用户输入，便于构造异常请求或验证字段差异。
std::string build_json_payload(const Options& options)
{
    if (!options.rawJson.empty()) {
        return options.rawJson;
    }

    nlohmann::json request;
    request["query"] = options.query;
    if (options.type == kKeywordRequest && !options.lang.empty()) {
        request["lang"] = options.lang;
    }
    if (options.hasTopK) {
        request["topk"] = options.topK;
    }
    return request.dump();
}

// TLV 编码格式：
//   1 byte type + 4 bytes length(big-endian/network order) + JSON bytes
// 这里手动按字节写 length，避免不同平台的整数内存布局影响结果。
std::vector<uint8_t> encode_tlv(uint8_t type, const std::string& value)
{
    if (value.size() > 0xffffffffu) {
        throw std::runtime_error("payload is too large");
    }

    uint32_t length = static_cast<uint32_t>(value.size());
    std::vector<uint8_t> packet;
    packet.reserve(5 + value.size());
    packet.push_back(type);
    packet.push_back(static_cast<uint8_t>((length >> 24) & 0xff));
    packet.push_back(static_cast<uint8_t>((length >> 16) & 0xff));
    packet.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
    packet.push_back(static_cast<uint8_t>(length & 0xff));
    packet.insert(packet.end(), value.begin(), value.end());
    return packet;
}

// 从 5 字节 TLV header 中解析 length。header[0] 是 type，后 4 字节是网络序长度。
uint32_t decode_length(const std::vector<uint8_t>& header)
{
    if (header.size() != 5) {
        throw std::runtime_error("TLV header must be 5 bytes");
    }
    return (static_cast<uint32_t>(header[1]) << 24)
        | (static_cast<uint32_t>(header[2]) << 16)
        | (static_cast<uint32_t>(header[3]) << 8)
        | static_cast<uint32_t>(header[4]);
}

// 以类似 hexdump -C 的格式打印原始字节：
// 左侧是偏移，中间是十六进制，右侧是可打印 ASCII 预览。
// 中文 UTF-8 字节在右侧通常显示为点号，这是正常现象。
void print_hex_dump(const std::vector<uint8_t>& bytes)
{
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
        std::cout << std::setw(6) << std::setfill('0') << std::hex << offset << "  ";

        for (std::size_t i = 0; i < 16; ++i) {
            if (offset + i < bytes.size()) {
                std::cout << std::setw(2) << static_cast<unsigned>(bytes[offset + i]) << ' ';
            } else {
                std::cout << "   ";
            }
            if (i == 7) {
                std::cout << ' ';
            }
        }

        std::cout << " |";
        for (std::size_t i = 0; i < 16 && offset + i < bytes.size(); ++i) {
            unsigned char ch = bytes[offset + i];
            std::cout << (std::isprint(ch) ? static_cast<char>(ch) : '.');
        }
        std::cout << "|\n";
    }
    std::cout << std::dec << std::setfill(' ');
}

// 响应 value 正常应该是 JSON。解析成功就缩进打印，解析失败则原样打印。
std::string pretty_json_or_raw(const std::string& text)
{
    try {
        return nlohmann::json::parse(text).dump(2);
    } catch (const std::exception&) {
        return text;
    }
}

// 使用 getaddrinfo 同时支持 IPv4、IPv6 和域名。返回的是已连接的 socket fd。
int connect_to_server(const std::string& host, const std::string& port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    int fd = -1;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        throw std::runtime_error("failed to connect to " + host + ":" + port);
    }
    return fd;
}

// TCP send 可能只写入部分数据，因此必须循环直到完整 TLV 包全部发出。
void send_all(int fd, const std::vector<uint8_t>& bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            throw std::runtime_error("send returned 0 bytes");
        }
        sent += static_cast<std::size_t>(n);
    }
}

// TCP recv 也可能只读到部分数据。TLV 解码必须先精确读 5 字节 header，
// 再根据 header 中的 length 精确读取 value。
std::vector<uint8_t> recv_exact(int fd, std::size_t size)
{
    std::vector<uint8_t> bytes(size);
    std::size_t received = 0;
    while (received < size) {
        ssize_t n = recv(fd, bytes.data() + received, size - received, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            throw std::runtime_error("connection closed before full response arrived");
        }
        received += static_cast<std::size_t>(n);
    }
    return bytes;
}
}

int main(int argc, char** argv)
{
    try {
        // 1. 解析命令行，构造 JSON value，再封装成 TLV 原始字节。
        Options options = parse_options(argc, argv);
        std::string payload = build_json_payload(options);
        std::vector<uint8_t> requestPacket = encode_tlv(options.type, payload);

        // 2. 发送前先打印“逻辑请求”和“真正写到 TCP 的原始字节”。
        std::cout << "========== TLV Request ==========\n";
        std::cout << "server: " << options.host << ':' << options.port << '\n';
        std::cout << "type: " << static_cast<unsigned>(options.type)
                  << " (" << type_name(options.type) << ")\n";
        std::cout << "length: " << payload.size() << " bytes\n";
        std::cout << "json value:\n" << pretty_json_or_raw(payload) << "\n\n";
        std::cout << "raw request bytes:\n";
        print_hex_dump(requestPacket);

        int fd = connect_to_server(options.host, options.port);
        send_all(fd, requestPacket);

        // 3. 读取响应时也严格按 TLV 边界处理：先 header，再 body。
        std::vector<uint8_t> header = recv_exact(fd, 5);
        uint8_t responseType = header[0];
        uint32_t responseLength = decode_length(header);
        std::vector<uint8_t> body = recv_exact(fd, responseLength);
        close(fd);

        std::vector<uint8_t> responsePacket = header;
        responsePacket.insert(responsePacket.end(), body.begin(), body.end());
        std::string responseText(body.begin(), body.end());

        // 4. 同时打印响应原始字节和解码后的 JSON，方便对照协议层和业务层。
        std::cout << "\n========== TLV Response ==========\n";
        std::cout << "type: " << static_cast<unsigned>(responseType)
                  << " (" << type_name(responseType) << ")\n";
        std::cout << "length: " << responseLength << " bytes\n";
        std::cout << "raw response bytes:\n";
        print_hex_dump(responsePacket);
        std::cout << "\ndecoded json value:\n" << pretty_json_or_raw(responseText) << '\n';

        // 服务端业务错误会以 type=100 返回。这里用退出码 2 区分“连接/客户端错误”。
        return responseType == kErrorResponse ? 2 : 0;
    } catch (const std::exception& ex) {
        std::cerr << "[tlv_client error] " << ex.what() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
}
