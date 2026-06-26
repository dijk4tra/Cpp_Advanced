#include "../../include/cache/RedisCache.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace
{
class SocketGuard
{
public:
    explicit SocketGuard(int fd) : fd_(fd) {}
    ~SocketGuard()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

timeval to_timeval(int milliseconds)
{
    milliseconds = milliseconds > 0 ? milliseconds : 20;
    timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    return tv;
}

std::string encode_command(const std::vector<std::string>& command)
{
    std::ostringstream oss;
    oss << '*' << command.size() << "\r\n";
    for (const auto& part : command) {
        oss << '$' << part.size() << "\r\n" << part << "\r\n";
    }
    return oss.str();
}
}

RedisCache::RedisCache(std::string host,
                       int port,
                       int db,
                       int connectTimeoutMs,
                       int commandTimeoutMs)
    : host_(std::move(host))
    , port_(port > 0 ? port : 6379)
    , db_(db >= 0 ? db : 0)
    , connectTimeoutMs_(connectTimeoutMs > 0 ? connectTimeoutMs : 20)
    , commandTimeoutMs_(commandTimeoutMs > 0 ? commandTimeoutMs : 20)
{
}

bool RedisCache::get(const std::string& key, std::string& value)
{
    Reply reply;
    if (!execute({"GET", key}, reply)) {
        return false;
    }

    if (reply.type != Reply::Type::BulkString) {
        return false;
    }

    value = std::move(reply.text);
    return true;
}

void RedisCache::put(const std::string& key, const std::string& value, int ttlSeconds)
{
    Reply reply;
    if (ttlSeconds > 0) {
        execute({"SETEX", key, std::to_string(ttlSeconds), value}, reply);
    } else {
        execute({"SET", key, value}, reply);
    }
}

void RedisCache::erase(const std::string& key)
{
    Reply reply;
    execute({"DEL", key}, reply);
}

bool RedisCache::execute(const std::vector<std::string>& command, Reply& reply) const
{
    try {
        int fd = connect_socket();
        if (fd < 0) {
            return false;
        }

        SocketGuard guard(fd);
        if (!select_db(fd)) {
            return false;
        }
        if (!send_command(fd, command)) {
            return false;
        }
        return read_reply(fd, reply) && reply.type != Reply::Type::Error && reply.type != Reply::Type::Invalid;
    } catch (const std::exception&) {
        return false;
    }
}

bool RedisCache::select_db(int fd) const
{
    if (db_ == 0) {
        return true;
    }

    Reply reply;
    return send_command(fd, {"SELECT", std::to_string(db_)})
        && read_reply(fd, reply)
        && reply.type == Reply::Type::SimpleString;
}

int RedisCache::connect_socket() const
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    int err = ::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &result);
    if (err != 0) {
        return -1;
    }

    int connected = -1;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) {
            connected = fd;
        } else if (errno == EINPROGRESS) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(fd, &writeSet);
            timeval tv = to_timeval(connectTimeoutMs_);
            rc = ::select(fd + 1, nullptr, &writeSet, nullptr, &tv);
            if (rc > 0) {
                int soError = 0;
                socklen_t len = sizeof(soError);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) == 0 && soError == 0) {
                    connected = fd;
                }
            }
        }

        if (connected >= 0) {
            if (flags >= 0) {
                ::fcntl(connected, F_SETFL, flags);
            }
            timeval tv = to_timeval(commandTimeoutMs_);
            ::setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            break;
        }

        ::close(fd);
    }

    ::freeaddrinfo(result);
    return connected;
}

bool RedisCache::send_command(int fd, const std::vector<std::string>& command) const
{
    return send_all(fd, encode_command(command));
}

bool RedisCache::send_all(int fd, const std::string& data) const
{
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t sent = ::send(fd, ptr, remaining, 0);
        if (sent <= 0) {
            return false;
        }
        ptr += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool RedisCache::read_reply(int fd, Reply& reply) const
{
    char prefix = '\0';
    if (read(fd, &prefix, 1) != 1) {
        return false;
    }

    std::string line;
    if (prefix == '+') {
        if (!read_line(fd, line)) {
            return false;
        }
        reply.type = Reply::Type::SimpleString;
        reply.text = line;
        return true;
    }

    if (prefix == '-') {
        if (!read_line(fd, line)) {
            return false;
        }
        reply.type = Reply::Type::Error;
        reply.text = line;
        return true;
    }

    if (prefix == ':') {
        if (!read_line(fd, line)) {
            return false;
        }
        reply.type = Reply::Type::Integer;
        reply.integer = std::stoll(line);
        return true;
    }

    if (prefix == '$') {
        if (!read_line(fd, line)) {
            return false;
        }
        long long size = std::stoll(line);
        if (size < 0) {
            reply.type = Reply::Type::Nil;
            return true;
        }

        std::string payload;
        if (!read_exact(fd, static_cast<std::size_t>(size), payload)) {
            return false;
        }
        std::string crlf;
        if (!read_exact(fd, 2, crlf) || crlf != "\r\n") {
            return false;
        }
        reply.type = Reply::Type::BulkString;
        reply.text = std::move(payload);
        return true;
    }

    reply.type = Reply::Type::Invalid;
    return false;
}

bool RedisCache::read_line(int fd, std::string& line) const
{
    line.clear();
    char ch = '\0';
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n != 1) {
            return false;
        }
        if (ch == '\r') {
            n = ::read(fd, &ch, 1);
            return n == 1 && ch == '\n';
        }
        line.push_back(ch);
    }
}

bool RedisCache::read_exact(int fd, std::size_t size, std::string& output) const
{
    output.assign(size, '\0');
    std::size_t readBytes = 0;
    while (readBytes < size) {
        ssize_t n = ::read(fd, output.data() + readBytes, size - readBytes);
        if (n <= 0) {
            return false;
        }
        readBytes += static_cast<std::size_t>(n);
    }
    return true;
}
