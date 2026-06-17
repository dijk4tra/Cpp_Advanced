// TCP编程:
// a. Daytime服务器(短链接)
// b. Echo服务器(长连接)
// c. Chatroom服务器(长连接, 连接之间有交互)

/*
 * Echo Server（基于 epoll 的 I/O 多路复用模型）
 *
 * 模型说明：
 *   - 使用 epoll 实现单线程并发处理多个 TCP 连接
 *   - 核心机制：事件驱动（event-driven）
 *
 * 处理流程：
 *   socket → bind → listen → epoll监听 → accept → epoll管理connfd → recv/send
 */

#include "common.h"
#include <arpa/inet.h>       // 提供了网络地址转换函数（如 inet_ntop）
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>      // 提供了 sockaddr_in 结构体及 htons 等网络字节序转换函数
#include <strings.h>         // 提供了 bzero
#include <sys/epoll.h>       // 提供了 epoll 系列核心 API
#include <sys/socket.h>      // 提供了 socket, bind, listen, accept 等套接字核心 API
#include <unistd.h>          // 提供了 close 资源回收函数

using namespace std;

// BACKLOG 是 Linux 内核中 ESTABLISHED 状态队列（已完成三次握手）的最大长度。
// 如果并发连接请求过多且服务器来不及 accept，超出此长度的连接请求将被拒绝或丢弃。
static const int BACKLOG = 128;

// MAXEVENTS 决定了每次 epoll_wait 最多能一次性带回多少个就绪事件。
// 它并不限制服务器的总连接数，只影响单次处理的批量效率。
static const int MAXEVENTS = 1024;

/*
 * 创建监听 socket 并进入 LISTEN 状态
 *
 * TCP 内核结构变化：
 *   socket()  → 未绑定状态（CLOSED）
 *   bind()    → 绑定本地地址
 *   listen()  → 进入 LISTEN 状态
 *               内核维护：
 *                 - SYN 半连接队列（SYN_RECV）
 *                 - accept 已完成队列（ESTABLISHED）
 */
int tcp_listen(uint16_t port)
{
    // 1. 创建 TCP socket（Socket Creation）
    // AF_INET: 使用 IPv4 协议族
    // SOCK_STREAM: 使用面向连接的可靠字节流（即 TCP 协议）
    // 0: 自动选择默认协议（此处即 IPPROTO_TCP）
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket()");
        exit(-1);
    }

    // 2. 设置地址复用 (SO_REUSEADDR)
    // 当服务器主动关闭时，处于 TIME_WAIT 状态的端口在 2MSL 时间内无法被再次绑定。
    // 开启此选项可以允许服务器程序崩溃或重启后，立即重新绑定该端口，避免出现 "Address already in use" 错误。
    int opt = 1; // 0 代表关闭. 1（或非 0 值）代表开启.
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定套接字地址 (Binding)
    struct sockaddr_in addr;
    // 将结构体内存清零，防止脏数据干扰
    bzero(&addr, sizeof(addr));
    // 指定地址族为 IPv4
    addr.sin_family = AF_INET;
    // htons: Host to Network Short，将本地字节序（通常是小端）转换为网络字节序（大端）
    addr.sin_port = htons(port);
    // INADDR_ANY: 监听本机所有网卡上的 IP 地址。htonl 同样用于字节序转换
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 通配符地址

    // bind 需要强转为通用套接字地址结构 `struct sockaddr*`
    int err = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (err) {
        perror("bind()");
        exit(-1);
    }

    // 4. 将通用套接字转变为“被动监听套接字” (Listening)
    // - 默认情况下，socket() 创建的只是一个通用的、未定向的套接字。
    // - listen() 的核心作用，就是明确告诉内核：“这个套接字不要用来发送或接收普通数据，
    //   请把它标记为【被动角色（Passive）】，专门用来监听和等待客户端的连接请求。”
    //
    // 一旦调用 listen()，内核就会开始为该套接字分配和维护两个核心队列：
    //   1. 半连接队列：存放收到 SYN 但尚未完成三次握手的连接（SYN_RCVD 状态）。
    //   2. 已连接队列：存放已完成三次握手、等待被 accept() 取走的连接（ESTABLISHED 状态，最大长度受 BACKLOG 限制）。
    err = listen(sockfd, BACKLOG);
    if (err) {
        perror("listen()");
        exit(-1);
    }
    cout << "Server is listening..." << endl;
    return sockfd;
}

int main()
{
    // 1. 初始化监听套接字
    // 绑定通配符地址, 监听 9999 端口
    int listenfd = tcp_listen(9999);

    // 2. 创建 epoll 实例
        // epoll_create(int size) 中的 size 参数在 Linux 2.6.8 之后已被忽略，
        // 但必须传入一个大于 0 的正数。内核会在内部创建一个由【红黑树】和【双向链表】组成的数据结构。
        //   - 红黑树：用于高效地添加、删除、修改需要监控的套接字（避免像 select 一样每次重复拷贝）
        //   - 双向链表：用于存放已经触发事件的就绪套接字。
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create()");
        exit(-1);
    }

    // 3. 将 listenfd 添加到 epoll 树中
    // 目的是：让内核通过 epoll 监控 listenfd 的 EPOLLIN 事件
    // 当完成三次握手的连接进入 accept 队列时，listenfd 变为可读状态
    // epoll_wait 会返回该事件，提示可以调用 accept() 获取新连接
    struct epoll_event evt;
    // 监听读事件（对于监听套接字来说，读就绪 = 有新连接到来）
    evt.events = EPOLLIN;
    // data 是用户自定义字段，用于保存与该 fd 关联的上下文信息
    // 当事件触发时，epoll 会将其原样返回，便于快速定位对应的连接或资源
    evt.data.fd = listenfd;
    // EPOLL_CTL_ADD: 往红黑树中挂载新节点
    int err = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &evt);
    if (err) {
        perror("Add listenfd to epoll instance");
        exit(-1);
    }

    // 4. 事件循环
    for (;;) {
        // 创建一个数组用于接收内核传回的就绪事件列表
        struct epoll_event events[MAXEVENTS];

        // epoll_wait 是一个阻塞函数：
        //   - 参数 -1 表示永久阻塞，直到有事件发生或者被信号中断才返回。
        //   - 返回值 nready 表示当前有多少个文件描述符处于就绪状态。
        int nready = epoll_wait(epfd, events, MAXEVENTS, -1);
        if (nready < 0) {
            // 如果是被系统信号中断（errno == EINTR），通常应该继续，这里简化处理直接退出
            perror("epoll_wait()");
            break;
        }

        // 逐个遍历所有已就绪的事件（只有就绪的才会被放入 events 数组，O(1) 效率极高）
        for (int i = 0; i < nready; ++i) {

            // 安全检查：如果触发的不是可读事件（安全防御代码），直接跳过
            if ((events[i].events & EPOLLIN) == 0) {
                continue;
            }

            // =============================================
            // 【情况 A】listenfd 就绪：说明有全新的客户端发起连接
            // =============================================
            if (events[i].data.fd == listenfd) {
                struct sockaddr_in cliaddr; // 存放客户端的 IP 和端口信息
                socklen_t length = sizeof(cliaddr); // 传入传出参数

                // accept 从已连接队列中取出一个连接：
                // 并会返回一个【全新的主动套接字 (connfd)】，专门用于后续与该客户端进行数据收发。
                int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &length);
                if (connfd == -1) {
                    perror("accept()");
                    // 某个连接失败，不影响服务器整体循环，继续处理其他事件
                    continue;
                }

                // 将新生成的负责通信的 connfd 挂载到 epoll 红黑树上，监控它的读事件
                // 默认是水平触发(LT)模式：只要缓冲区有数据没读完，epoll_wait 就会一直触发。
                evt.events = EPOLLIN;
                evt.data.fd = connfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &evt);

                // 打印新连接的日志（将网络字节序转换为人类易读的字符串）
                char ipstr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cliaddr.sin_addr, ipstr, INET_ADDRSTRLEN); // 转换 IP
                uint16_t port = ntohs(cliaddr.sin_port);                       // 转换端口
                printf("新连接建立，客户端地址为 %s:%hu\n", ipstr, port);
            } else {
                // ==============================================
                // 【情况 B】通信套接字 connfd 就绪：说明老客户端有变动
                //  可能的原因包括：
                //   - 接收缓冲区中有新数据到达
                //   - 对端关闭连接（FIN，recv 返回 0)
                // ==============================================
                char buf[4096];
                // 获取当前触发 I/O 事件的客户端通信文件描述符 (connfd)
                int connfd = events[i].data.fd;
                // 从该套接字的内核接收缓冲区 (Receive Buffer) 中读取数据
                int nbytes = recv(connfd, buf, sizeof(buf), 0);

                if (nbytes < 0) {
                    // 1. 发生读取错误
                    perror("read()");
                    // 将 connfd 从 epoll 实例中删除 (不再监听)
                    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL);
                    close(connfd);
                } else if (nbytes == 0) {
                    // 2. 客户端主动断开连接 (收到 FIN 包)
                    // 在 TCP 协议中，对方调用 close() 或是进程退出时，recv 会返回 0
                    // 将 connfd 从 epoll 实例中删除（不再监听）
                    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL);
                    close(connfd); // 释放文件描述符
                    printf("客户端断开连接，关闭文件描述符%d\n", connfd);
                } else {
                    // 3. 真正接收到了客户端的数据
                    // 业务逻辑处理：遍历缓冲区，将小写字母改为大写字母
                    for (int j = 0; j < nbytes; ++j) {
                        buf[j] = toupper(buf[j]); // 将字母转为大写
                    }

                    // 将大写处理后的数据原路发送回客户端
                    // 注意：在生产环境的高并发代码中，send 应该放入 EPOLLOUT 事件中发送，
                    // 这里由于是教学演示且数据量极小，直接同步 send 简化了逻辑。
                    send(connfd, buf, nbytes, 0);

                    // 终端打印回显数据日志：%.*s 表示精准输出指定长度的字符串，防止因为没有 '\0' 而导致内存越界打印
                    printf("%.*s", nbytes, buf); // 只输出前 nbytes 个字符
                }
            }
        } // end of for (nready)
    } // end of for (;;)

// 清理标签（本代码中上面是死循环，实际生产中应配合信号捕获，在此处做优雅退出清理
end:
    close(listenfd);
    return 0;
}
