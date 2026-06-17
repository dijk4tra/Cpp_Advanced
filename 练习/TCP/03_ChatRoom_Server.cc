// Chatroom Server（基于 epoll 的聊天室服务器）
//
// 业务特点：
//   - 使用 epoll 单线程管理多个 TCP 长连接
//   - 每个客户端连接后不会立即断开
//   - 某个客户端发送消息后，服务器将消息广播给其他客户端
//
// 处理流程：
//   socket → bind → listen → epoll监听 → accept → epoll管理connfd → recv → broadcast

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <signal.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>

using namespace std;

// 已完成三次握手、等待 accept 的连接队列最大长度
static const int BACKLOG = 128;

// epoll_wait 每次最多返回的就绪事件数量
static const int MAXEVENTS = 1024;

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

    // 3. 绑定本地地址
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int err = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (err) {
        perror("bind()");
        exit(-1);
    }

    // 4. 进入监听状态
    err = listen(sockfd, BACKLOG);
    if (err) {
        perror("listen()");
        exit(-1);
    }

    cout << "Chatroom Server is listening on port " << port << "..." << endl;
    return sockfd;
}

/*
 * 从 epoll 中删除客户端，并关闭对应的通信套接字
 */
void remove_client(int epfd, unordered_set<int>& clients, int connfd)
{
    // 从 epoll 红黑树中删除 connfd
    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, nullptr);

    // 从客户端集合中输出connfd
    clients.erase(connfd);

    // 关闭通信套接字
    close(connfd);

    printf("客户端断开连接，关闭文件描述符：%d，当前在线人数：%zu\n",
           connfd,
           clients.size());
}

/*
 * 将 senderfd 发来的消息广播给其他客户端
 */
void broadcast_message(const unordered_set<int>& clients,
                       int senderfd,
                       const char* buf,
                       int nbytes)
{
    char msg[8192];

    // 给消息加上发送者fd,以便于区分是谁发来的消息
    // snprintf 用来把格式化后的字符串写入 msg 缓冲区中。
    // 相比 sprintf，snprintf 多了一个缓冲区大小参数 sizeof(msg)，可以防止写越界
    int msg_len = snprintf(msg, // 目标缓冲区：格式化后的聊天消息最终会写入 msg 数组中
                           sizeof(msg), // 目标缓冲区的最大容量
                           "[client %d] %.*s",
                           senderfd, // 对应格式中的 %d，表示发送消息的客户端文件描述符
                           nbytes,   // 对应格式中的 *，表示只从 buf 中取 nbytes 个字节
                           buf);     // 对应格式中的 s，表示真正接收到的客户端消息内容

    if (msg_len <= 0) {
            return;
        }

    // 防止消息过长导致越界
    if (msg_len >= (int)sizeof(msg)) {
        msg_len = sizeof(msg) - 1;
    }

    // 遍历所有在线客户端
    for (int clientfd : clients){
        // 发消息给除自己外的客户端
        if (clientfd == senderfd){
            continue;
        }

        // 生产环境中更严谨的做法是把待发送数据放入发送缓冲区，
        // 然后监听 EPOLLOUT 事件，等套接字可写时再发送。
        // MSG_NOSIGNAL是为了防止当前进程因为向一个断开的连接发送数据而崩溃
        send(clientfd, msg, msg_len, MSG_NOSIGNAL);
    }
}

int main()
{
    // 忽略 SIGPIPE，防止向已关闭连接 send 时进程被系统杀死
    signal(SIGPIPE, SIG_IGN);

    // 1. 初始化监听套接字
    int listenfd = tcp_listen(9998);

    // 2. 创建 epoll 实例
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create()");
        exit(-1);
    }

    // 3. 将 listenfd 添加到 epoll 中
    struct epoll_event evt;
    evt.events = EPOLLIN;
    evt.data.fd = listenfd;

    int err = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &evt);
    if (err) {
        perror("epoll_ctl ADD listenfd");
        exit(-1);
    }

    // 保存所有在线客户端的 connfd
    unordered_set<int> clients;

    // 4. 事件循环
    for (;;) {
        struct epoll_event events[MAXEVENTS];

        // 等待事件发生
        int nready = epoll_wait(epfd, events, MAXEVENTS, -1);
        if (nready < 0) {
            perror("epoll_wait()");
            break;
        }

        // 遍历所有就绪事件
        for (int i = 0; i < nready; i++) {
            // 只处理可读事件
            if ((events[i].events & EPOLLIN) == 0) {
                continue;
            }

            // =====================================================
            // 情况 A：listenfd 可读，说明有新客户端连接
            // =====================================================
            if (events[i].data.fd == listenfd) {
                struct sockaddr_in cliaddr;
                socklen_t length = sizeof(cliaddr);

                int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &length);
                if (connfd == -1) {
                    perror("accept()");
                    continue;
                }

                // 将新客户端 connfd 加入 epoll
                evt.events = EPOLLIN;
                evt.data.fd = connfd;

                int ret = epoll_ctl(epfd,EPOLL_CTL_ADD, connfd, &evt);
                if (ret == -1) {
                    perror("epoll_ctl ADD connfd");
                    close(connfd);
                    continue;
                }

                // 加入在线客户端集合
                clients.insert(connfd);

                // 打印客户端集合
                char ipstr[INET_ADDRSTRLEN];
                // Internet Network to Presentation: 网络二进制格式转为可读表现形式
                inet_ntop(AF_INET, &cliaddr.sin_addr, ipstr, INET_ADDRSTRLEN);
                uint16_t port = ntohs(cliaddr.sin_port);

                printf("新客户端加入聊天室：%s:%hu，connfd = %d，当前在线人数：%zu\n",
                       ipstr,
                       port,
                       connfd,
                       clients.size());

                // 给新客户端发送欢迎消息
                const char* welcome = "Welcome to chatroom!\n";
                send(connfd, welcome, strlen(welcome), MSG_NOSIGNAL);
            }

            // =====================================================
            // 情况 B：某个 connfd 可读，说明老客户端发来了消息
            // =====================================================
            else {
                int connfd = events[i].data.fd;

                char buf[4096];

                // 从客户端接收消息
                int nbytes = recv(connfd, buf, sizeof(buf), 0);

                if (nbytes < 0) {
                    // recv 出错, 关闭该客户端
                    perror("recv()");
                    remove_client(epfd, clients, connfd);
                } else if (nbytes == 0) {
                    // recv 返回 0，表示客户端主动关闭连接
                    remove_client(epfd, clients, connfd);
                } else {
                    // 收到正常聊天消息
                    printf("[cliend %d] %.*s", connfd, nbytes, buf);

                    // 将该消息广播给其他客户端
                    broadcast_message(clients, connfd, buf, nbytes);
                }
            }
        }
    }

    // 程序退出前清理资源
    for (int connfd : clients) {
        close(connfd);
    }

    close(listenfd);
    close(epfd);

    return 0;
}
