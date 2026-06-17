#include "common.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

#define BACKLOG 128    // ESTABLISHED 状态队列（已完成三次握手）的最大长度
#define MAXEVENTS 1024 // 每次 epoll_wait 最多能一次性带回多少个就绪事件

int tcp_listen(uint16_t port)
{
    // 1. 创建主动套接字 (Socket Creation)
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket()"); // 打印系统错误信息
        exit(-1);
    }

    // 2. 设置地址复用 (SO_REUSEADDR)
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定套接字地址 (Binding)
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int err = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (err) {
        perror("bind()");
        exit(-1);
    }

    // 4. 将主动套接字转变为被动监听套接字 (Listening)
    err = listen(sockfd, BACKLOG);
    err = listen(sockfd, BACKLOG);
    if (err) {
        perror("listen()");
        exit(-1);
    }
    cout << "Server is listening on port " << port << "..." << endl;
    return sockfd;
}

int main ()
{
    // 1. 初始化监听套接字
    int listenfd = tcp_listen(9999);

    // 2. 创建 epoll 实例
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create()");
        exit(-1);
    }

    // 3. 将 listenfd 添加到 epoll 树中
    struct epoll_event evt;
    evt.events = EPOLLIN;
    evt.data.fd = listenfd;

    int err = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &evt);
    if (err) {
        perror("Add listenfd to epoll instance");
        exit(-1);
    }

    // 4. 事件循环
    for (;;) {
        // 创建一个数组用于接收内核传回的就绪事件列表
        struct epoll_event events[MAXEVENTS];

        int nready = epoll_wait(epfd, events, MAXEVENTS, -1);
        if (nready < 0) {
            perror("epoll_wait()");
            break;
        }

        // 逐个遍历所有已就绪的事件
        for (int i = 0; i < nready; i++) {

            // 安全检查：如果触发的不是可读事件，直接跳过
            if ((events[i].events & EPOLLIN) == 0) {
                continue;
            }

            // 【情况 A】listenfd 就绪：说明有全新的客户端发起连接
            if (events[i].data.fd == listenfd) {
                struct sockaddr_in cliaddr;
                socklen_t length = sizeof(cliaddr);

                // 从已连接队列中取出一个连接
                int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &length);
                if (connfd == -1) {
                    perror("accept()");
                    continue;
                }

                // 将新生成的负责通信的 connfd 挂载到 epoll 红黑树上，监控它的读事件
                evt.events = EPOLLIN;
                evt.data.fd = connfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &evt);

                // 打印新连接的日志
                char ipstr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cliaddr.sin_addr, ipstr, INET_ADDRSTRLEN);
                uint16_t port = ntohs(cliaddr.sin_port);
                printf("[LOG] 新连接建立，客户端地址为 %s:%hu, 分配的 FD=%d\n", ipstr, port, connfd);
            }
            // ==============================================
            // 【情况 B】通信套接字 connfd 就绪：说明老客户端有变动
            //  可能的原因包括：
            //   - 接收缓冲区中有新数据到达
            //   - 对端关闭连接（FIN，recv 返回 0)
            // ==============================================
            else {
                char buf[4096];
                // 获取当前触发 I/O 事件的客户端通信文件描述符
                int connfd = events[i].data.fd;
                // 从该套接字的内核接收缓冲区中读取数据
                int nbytes = recv(connfd, buf, sizeof(buf), 0);
                if (nbytes < 0) {
                    // 1. 发生读取错误
                    perror("recv()");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL);
                    close(connfd);
                }
                else if (nbytes == 0) {
                    // 2. 客户端主动断开连接
                    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL);
                    close(connfd);
                    printf("[LOG] 客户端主动断开连接，关闭文件描述符 FD=%d\n", connfd);
                }
                else {
                    // 3. 真正接收到了客户端的数据
                    for (int j = 0; j < nbytes; j++) {
                        buf[j] = toupper(buf[j]);
                    }
                    send(connfd, buf, nbytes, 0);
                    printf("[Echo back to FD=%d]: %.*s", connfd, nbytes, buf);
                }
            }
        }
    }
end:
    close(listenfd);
    return 0;
}
