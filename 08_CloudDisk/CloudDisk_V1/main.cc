#include "CloudDiskServer.h"
#include <iostream>
#include <signal.h>

using namespace std;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);
    srand(time(NULL)); // 设置随机种子

    CloudDiskServer server;

    // 注册路由
    server.register_routes();

    // server.start();

    if (server.start(8888) == 0) {
        // server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
    }
}
