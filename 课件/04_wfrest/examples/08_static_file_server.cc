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
    signal(SIGINT, sig_handler); // 注册Ctrl+C信号处理

    HttpServer server;

    // 2. 设置路由
    // server.GET("/static/*", [](const HttpReq* req, HttpResp* resp) {
    //     string parent = "./www/static/";
    //     string file = req->match_path();
    //     resp->File(parent + file);
    // });

    // url.path --> filepath(目录，文件)
    server.Static("/static", "./www/static");
    server.Static("/public", "./www");
    server.Static("/", "./www/index.html");

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
