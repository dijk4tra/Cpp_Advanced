/**
 * [[回声服务器]]
 * 我们将使用Workflow创建一个HTTP服务器，它会将接收到的HTTP请求，以html格式返回给客户端。
 * 此外，它还需要完成下面这些功能：
 *   - 程序会打印出客户端的IP地址，请求序号（当前TCP连接上的第几次请求）。
 *   - 当一个TCP连接完成了10次请求，服务器主动关闭连接。
 *   - 按下Ctrl + C程序能够正常结束，资源都会被回收。
 */
#include "common.h"
#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFServer.h>

using namespace std;
using namespace protocol;

void process(WFHttpTask* task)
{
    // 1. 获取客户端的套接字地址
    struct sockaddr_storage addr;
    socklen_t length = sizeof(addr);
    task->get_peer_addr((struct sockaddr*)&addr, &length);
    // 2. 解析套接字地址
    char ipstr[128];
    unsigned short port;
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
        inet_ntop(AF_INET, &sin->sin_addr, ipstr, 128);
        port = ntohs(sin->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipstr, 128);
        port = ntohs(sin6->sin6_port);
    } else {
        strcpy(ipstr, "Unknown address family");
    }
    cout << "客户端地址：" << ipstr << ":" << port << endl;
    // 3. 打印请求序号（从0编号）
    long long seq = task->get_task_seq();
    cout << "请求序号：" << seq << endl;
    // 4. 解析请求 --> [处理业务逻辑] --> 生成响应（根据处理结果）
    HttpRequest* req = task->get_req();
    HttpResponse* resp = task->get_resp();
    // 响应行
    resp->set_http_version("HTTP/1.1");
    resp->set_status_code("200");
    resp->set_reason_phrase("OK");
    // 响应头部
    resp->add_header_pair("Content-Type", "text/html");
    resp->add_header_pair("Server", "My WFHttpServer");
    if (seq == 9) {
        // 如果是第10次请求，将Connection设置为close（服务器主动断开连接）
        resp->set_header_pair("Connection", "close");
    }
    // 响应主体
    resp->append_output_body_nocopy("<html>"); // 静态存储期限：与天地同寿
    string line;
    line = line + "<p>" + req->get_method() + " "
        + req->get_request_uri() + " "
        + req->get_http_version() + "</p>";
    resp->append_output_body(line);
    // 遍历请求头部
    HttpHeaderCursor cursor(req);
    string name;
    string value;
    while (cursor.next(name, value)) {
        string header = "<p>" + name + ": " + value + "</p>";
        resp->append_output_body(header);
    }
    resp->append_output_body_nocopy("</html>");
}

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
}

int main(int argc, char* argv[])
{
    // 1. 注册信号处理函数
    signal(SIGINT, sig_handler);
    // 2. 创建HTTP服务器
    WFHttpServer server(process);
    // 3. 启动服务器：绑定通配符地址，监听在8888端口
    if (server.start(8888) == 0) {
        waitGroup.wait();
        server.stop(); // 让服务器优雅退出
    } else {
        cerr << "ERROR: Server start failed!" << endl;
        exit(1);
    }
    return 0;
}
