#include "ChatClient.h"
#include <iostream>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <pthread.h>

using namespace std;
using namespace muduo;
using namespace muduo::net;

// 输入线程函数：独立线程读取用户输入
void* inputThreadFunc(void* args)
{
    ChatClient* client = static_cast<ChatClient*>(args);
    string line;
    while (getline(cin, line)) { // 阻塞等待用户输入
        if (line == "/quit") {
            client->disconnect(); // 用户输入退出命令
            break;
        }
        if (!line.empty()) {
            client->send(line); // 发送用户输入的消息
        }
    }
    return NULL;
}

int main(int argc, char* argv[])
{
    if (argc < 3) {
        cout << "Usage: " << argv[0] << " <server_ip> <port>" << endl;
        return 1;
    }
    // 设置日志级别为INFO
    Logger::setLogLevel(Logger::INFO);

    // 创建事件循环（主线程运行）
    EventLoop loop;
    // 解析服务器地址和端口
    InetAddress serverAddr(argv[1], static_cast<uint16_t>(atoi(argv[2])));
    // 创建聊天客户端
    ChatClient client(&loop, serverAddr);
    // 连接服务器
    client.connect();

    // 启动输入线程，避免阻塞网络事件循环
    pthread_t tid;
    pthread_create(&tid, NULL, &inputThreadFunc, (void*)&client);

    // 进入事件循环（阻塞，直到loop->quit()被调用）
    loop.loop();

    return 0;
}
