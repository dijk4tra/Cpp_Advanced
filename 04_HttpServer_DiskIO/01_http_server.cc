#include <iostream>
#include <signal.h>
#include <workflow/HttpMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTaskFactory.h>

using namespace std;
using namespace protocol;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
}

void process(WFHttpTask* task)
{
    // 解析请求 --> [处理业务逻辑] --> 生成响应（根据处理结果）
    HttpResponse* resp = task->get_resp();

    // 解析请求
    HttpRequest* req = task->get_req();
    string method = req->get_method();
    string uri = req->get_request_uri();
    string version = req->get_http_version();
    // cout << "请求方法：" << method << " 请求URI：" << uri << " HTTP版本：" << version << endl;
    // 获取用户输入的路径
    auto pos = uri.find('?');
    string path = uri.substr(0, pos);
    // cout << "path: " << path << endl;

    // 分发请求
    if (method == "GET") {
        if (path == "/hello") {
            resp->append_output_body("Hello, World!\n");
        } else if (path == "/lover") {
            resp->append_output_body("Peanut loves jingtian\n");
        } else {
            resp->set_status_code("404");
            resp->append_output_body("404 NOT FOUND\n");
        }
    }
}


int main(int argc, char* argv[])
{
    // 1. 注册信号处理函数
    signal(SIGINT, sig_handler);

    // 2. 创建 HTTP 服务器
    WFHttpServer server(process);

    // 3. 启动服务器: 绑定通配符地址, 监听在8888端口
    if (server.start(8888) == 0)
    {
        // 让主线程阻塞
        waitGroup.wait();
        server.stop(); // 让服务器优雅退出
    } else {
        cerr << "ERROR: Server start failed" << endl;
        exit(1);
    }

    return 0;
}
