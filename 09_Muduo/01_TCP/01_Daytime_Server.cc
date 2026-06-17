// Daytime Server（短连接服务器）
//
// 业务特点：
//   - 客户端连接服务器
//   - 服务器发送当前时间
//   - 服务器立即关闭连接
//
// 处理流程：
//   socket → bind → listen → accept → send → close

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

// 已完成三次握手、等待 accept 的连接队列最大长度
static const int BACKLOG = 128;

/*
 * 创建监听 socket 并进入 LISTEN 状态
 */
int tcp_listen(uint16_t port)
{
    // 1. 创建 TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket()");
        exit(-1);
    }

    // 2. 设置地址复用
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定本地 IP 和端口
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int err = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (err) {
        perror("bind()");
        exit(-1);
    }

    // 4. 将 socket 转换为被动监听套接字
    err = listen(sockfd, BACKLOG);
    if (err) {
        perror("listen()");
        exit(-1);
    }

    cout << "Daytime Server is listening on port " << port << "..." << endl;
    return sockfd;
}

/*
 * 生成当前时间字符串
 *
 * 例如：
 *   2026-06-17 20:30:15
 */
void make_daytime_string(char* buf, size_t size)
{
    // 获取当前时间戳
    time_t now = time(nullptr);

    // 将时间戳转换为成本地时间结构体
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    // 格式化时间戳
    strftime(buf, size, "%Y-%m-%d %H:%M:%S\r\n", &tm_now);
}

int main()
{
    // 监听 9997 端口
    int listenfd = tcp_listen(9997);

    for (;;) {
        struct sockaddr_in cliaddr;
        socklen_t length = sizeof(cliaddr);

        // accept 会阻塞, 直到有客户端完成三次握手
        int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &length);
        if (connfd == -1) {
            perror("accept()");
            continue;
        }

        // 打印客户端地址信息
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, ipstr, INET_ADDRSTRLEN);
        uint16_t port = ntohs(cliaddr.sin_port);
        printf("客户端连接成功：%s:%hu\n", ipstr, port);

        // 生产当前时间
        char buf[128];
        make_daytime_string(buf, sizeof(buf));

        // 将时间发送给客户端
        send(connfd, buf, strlen(buf), 0);

        // Daytime 是短连接服务器，发送完数据后立即关闭连接
        close(connfd);

        printf("时间已发送，关闭连接：%s:%hu\n", ipstr, port);
    }

    close(listenfd);
    return 0;
}
