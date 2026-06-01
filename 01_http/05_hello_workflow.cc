#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/WFHttpServer.h>

using namespace std;
using namespace protocol;

int main()
{
    // 1. 创建HTTP服务器
    WFHttpServer server([](WFHttpTask* task) {
        HttpResponse* resp = task->get_resp();
        resp->append_output_body("<html>Hello World!</html>");
    });

    // 2. 启动服务器（绑定通配符地址），监听8888端口
    if (server.start(8888) == 0) {
        getchar(); // 按任意键退出
        server.stop(); // 优雅退出
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

}