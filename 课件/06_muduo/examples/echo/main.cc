#include "echo.h"
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

using namespace muduo;
using namespace muduo::net;

int main()
{
    // 1. 创建事件循环对象（Reactor 的核心）
    EventLoop loop;
    // 2. 监听的地址：端口 2007，IP 为任意地址（0.0.0.0）
    InetAddress listenAddr(2007);
    // 3. 创建 EchoServer 实例，传入 loop 和地址
    EchoServer server(&loop, listenAddr);
    // 4. 启动服务器（开始监听）
    server.start();
    // 5. 进入事件循环，直到loop被退出
    loop.loop();
    return 0; // 正常情况下不会执行到这里
}
