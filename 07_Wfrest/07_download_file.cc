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
        // 支持绝对路径, 但一般不要使用
        resp->File("/home/lws/my_project/Cpp_Advanced/07_Wfrest/a.txt");
    });

    server.GET("/download-file2", [](const HttpReq* req, HttpResp* resp) {
        // 一般使用相对路径
        resp->File("resources/b.txt");
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
