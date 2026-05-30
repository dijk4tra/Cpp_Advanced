#include "daytime.h"
#include <muduo/net/EventLoop.h> // 事件循环
#include <muduo/net/InetAddress.h> // 网络地址封装

using namespace muduo;
using namespace muduo::net;

int main()
{
    // 1. 创建事件循环（Reactor 核心）
    EventLoop loop;
    // 2. 创建监听地址：监听所有网卡（0.0.0.0）的 2013 端口
    InetAddress listenAddr(2013);
    // 3. 创建 Daytime 服务器，传入事件循环和监听地址
    DaytimeServer server(&loop, listenAddr);
    // 4. 启动服务器（开始监听）
    server.start();
    // 5. 进入事件循环（阻塞运行，直到 loop 被停止）
    loop.loop();
    return 0; // 正常情况下不会执行到这里（除非 loop 退出）
}
