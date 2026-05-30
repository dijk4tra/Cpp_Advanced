#include "ChatServer.h"
#include <iostream>
#include <memory>
#include <muduo/base/LogFile.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

using namespace std;
using namespace muduo;
using namespace muduo::net;

int main(int argc, char* argv[])
{
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <port>" << endl;
        return 1;
    }

    // 设置日志级别
    Logger::setLogLevel(Logger::INFO);

    // 创建事件循环
    EventLoop loop;
    // 设置监听地址和端口
    uint16_t port = static_cast<uint16_t>(atoi(argv[1]));
    InetAddress serverAddr(port);
    // 创建聊天服务器
    ChatServer server(&loop, serverAddr);
    // 启动服务器
    server.start();
    // 进入事件循环
    loop.loop();

    return 0;
}
