/**
 * [[回声服务器]]
 * 我们将使用Workflow创建一个HTTP服务器，它会将接收到的HTTP请求，以html格式返回给客户端。
 * 此外，它还需要完成下面这些功能：
 *   - 程序会打印出客户端的IP地址，请求序号（当前TCP连接上的第几次请求）。
 *   - 当一个TCP连接完成了10次请求，服务器主动关闭连接。
 *   - 按下Ctrl + C程序能够正常结束，资源都会被回收。
 */

// 套接字地址:
//     struct sockaddr: 通用套接字地址
//     struct sockaddr_in: IPv4套接字地址, family = AF_INET
//     struct sockaddr_in6: IPv6套接字地址, family = AF_INET6
//     struct sockaddr_storage: 存储对端地址
//
#include "common.h"
#include <arpa/inet.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/RedisMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFServer.h>

using namespace std;
using namespace protocol;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
}


void process(WFHttpTask* task)
{
    // 1. 获取客户端的套接字地址(对端地址)
    struct sockaddr_storage addr;
    socklen_t length = sizeof(addr); // 传入传输参数
    task->get_peer_addr((struct sockaddr*)&addr, &length);

    // 2. 解析套接字地址 --> IP:port
    char ipstr[INET6_ADDRSTRLEN];
    unsigned short port;
    if (addr.ss_family == AF_INET) {
        // IPv4地址
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
        // ntop: network to presentation 将二进制的IP地址转换为字符串形式
        inet_ntop(AF_INET, &sin->sin_addr, ipstr, INET6_ADDRSTRLEN);
        // ntohs: network to host short 将网络字节序的端口号转换为主机字节序
        port = ntohs(sin->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        // IPv6地址
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipstr, INET6_ADDRSTRLEN);
        port = ntohs(sin6->sin6_port);
    } else {
        strcpy(ipstr, "Unknown address family");
    }
    cout << "客户端地址：" << ipstr << ":" << port << endl;

    // 3. 打印请求序号（从0开始编号）
    long long seq = task->get_task_seq();
    cout << "请求序号：" << seq << endl;

    // 4. 解析请求 --> [处理业务逻辑] --> 生成响应（根据处理结果）
    HttpRequest* req = task->get_req();
    HttpResponse* resp = task->get_resp();

    // 5. 生成响应（根据处理结果）
    // 设置响应行
    resp->set_http_version("HTTP/1.1");
    resp->set_status_code("200");
    resp->set_reason_phrase("OK");

    //  设置响应头
    resp->add_header_pair("Content-Type", "text/html");
    resp->add_header_pair("Server", "My WFHttpServer");
    if (seq == 9) {
        // 如果是第10次请求，将Connection设置为close（服务器主动断开连接）
        resp->set_header_pair("Connection", "close"); // close(sockfd) 关闭TCP链接
    }

    // 设置响应体
    // 要特别小心变量的生命周期, "<html>"会被最后的httpTask使用
    // resp->append_output_body("<html>"); // 会把 "<html>" 拷贝进响应体, 一般情况下更安全
    resp->append_output_body_nocopy("<html>"); // "<html>"具有静态存储期限
    string line; // 局部变量: 自动存储期限
    // 使用 + 进行字符串的拼接
    line = line + "<p>" + req->get_method() + " "
        + req->get_request_uri() + " "
        + req->get_http_version() + "</p>";
    // resp->append_output_body_nocopy(line); // ERROR: line 是局部变量, 可能已经被销毁
    resp->append_output_body(line); // 会把 line 拷贝进响应体

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
