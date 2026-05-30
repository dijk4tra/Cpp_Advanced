#include <iostream>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    // 1. 注册信号处理函数（按Ctrl+C退出）
    signal(SIGINT, sig_handler);
    // 2. 使用默认参数，创建HttpServer，开箱即用
    HttpServer server;
    // 3. 设置路由(相当于在process()函数中设置一个分支)
    // 静态路由：
    server.GET("/user/xixi", [](const HttpReq* req, HttpResp* resp) {
        resp->String("特别的爱给特别的你，我的寂寞逃不过你的眼睛\n");
    });
    // 参数路由：
    // 此路由将匹配 /user/peanut，但不匹配 /user/xixi 或 /user/
    server.GET("/user/{name}", [](const HttpReq* req, HttpResp* resp) {
        // 获取路径参数
        const string& name = req->param("name");
        cout << "name: " << name << endl;
        // 设置返回状态码
        resp->set_status(HttpStatusOK); // 默认返回HttpStatusOK: 200
        resp->String("Hello " + name + "\n");
    });
    // 通配符路由：
    server.GET("/wildcard/*", [](const HttpReq* req, HttpResp* resp) {
        // 解析路径
        cout << "match_path: " << req->match_path() << endl;
        cout << "full_path: " << req->full_path() << endl;
        cout << "current_path: " << req->current_path() << endl;
    });
    // 参数和通配符可以一起使用
    server.GET("/user/{name}/*", [](const HttpReq* req, HttpResp* resp) {
        string name = req->param("name");
        cout << "name: " << name << endl;
        cout << "match_path: " << req->match_path() << endl;
        cout << "full_path: " << req->full_path() << endl;
        cout << "current_path: " << req->current_path() << endl;
    });
    // 4. 启动服务器
    if (server.start(8888) == 0) {
        server.list_routes(); // 打印所有注册的路由
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
