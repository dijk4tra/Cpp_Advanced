#include <iostream>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;

// 用于优雅退出的全局等待组
static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    // 1. 注册信号处理函数（按Ctrl+C退出）
    signal(SIGINT, sig_handler);
    // 2. 使用默认参数创建服务器，开箱即用
    HttpServer server;
    // 3. 定义路由和处理逻辑
    server.GET("/hello", [](const HttpReq* req, HttpResp* resp) {
        resp->String("world\n"); // 返回一个简单的字符串
    });

    server.POST("/echo", [](const HttpReq* req, HttpResp* resp) {
        // 直接返回请求的主体，实现echo服务
        resp->String(req->body());
    });
    // 3. 启动服务器，绑定通配符地址，监听8888端口
    if (server.start(8888) == 0) {
        waitGroup.wait(); // 主线程等待，直到收到退出信号
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
}
