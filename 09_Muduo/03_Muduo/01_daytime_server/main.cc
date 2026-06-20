#include "daytime.h"

#include <muduo/net/EventLoop.h> // 事件循环
#include <muduo/net/InetAddress.h> // 网络地址封装

using namespace muduo;
using namespace muduo::net;

int main()
{
    // 1. 创建事件循环
    EventLoop loop;

    // 2. 创建套接字地址(封装套接字地址: struct sockaddr_in)
    InetAddress listenAddr(2013);

    // 3. 创建 Daytime 服务器，传入事件循环和监听地址
    DaytimeServer server(&loop, listenAddr);

    // 4. 启动服务器: 将监听套接字放入 epoll 实例中
    server.start();

    // 5. 进入事件循环（阻塞运行，直到 loop 被停止）
    loop.loop();

    return 0; // 正常情况下不会执行到这里（除非 loop 退出）
}
