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
    signal(SIGINT, sig_handler);

    HttpServer server;

    server.GET("/download-file1", [](const HttpReq* req, HttpResp* resp) {
        // 支持绝对路径，但一般不要使用
        resp->File("/home/peanut/a.txt");
    });

    server.GET("/download-file2", [](const HttpReq* req, HttpResp* resp) {
        // 一般使用相对路径
        resp->File("resources/a.txt");
    });

    server.GET("/download-file3", [](const HttpReq* req, HttpResp* resp) {
        // 支持范围请求
        resp->File("resources/a.txt", 6);
    });

    server.GET("/download-file4", [](const HttpReq* req, HttpResp* resp) {
        // 支持范围请求
        resp->File("resources/a.txt", 6, 11);
    });

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: cannot start server!\n";
        exit(1);
    }
    return 0;
}

/**
 * 思考：
 * 如果我们能通过用户传入的路径得知他想要哪个文件
 * 那么我们就可以用 resp->File() 轻易地实现一个静态资源服务器。
 */
