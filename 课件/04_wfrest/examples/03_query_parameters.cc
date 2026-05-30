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
    // 2. 创建HTTP服务器
    HttpServer server;
    // 3. 定义路由
    // URL: /all_queries?username=peanut&password=lovesxixi
    server.GET("/all_queries", [](const HttpReq* req, HttpResp* resp) {
        const map<string, string>& all_queries = req->query_list();
        // const auto& all_queries = req->query_list();   // 熟悉之后可以这样使用
        // 结构体绑定：C++17特性
        for (const auto& [name, value] : all_queries) {
            cout << name << ": " << value << endl;
        }
    });

    // URL: /query?username=peanut&passwd=lovesxixi
    server.GET("/query", [](const HttpReq* req, HttpResp* resp) {
        const string& username = req->query("username");
        const string& password = req->query("password");
        // 没有info参数，返回空字符串
        const string& info = req->query("info");
        assert(info == "" && "info doesn't equal \"\"");
        // 没有address参数，返回默认值"china"
        const string& address = req->default_query("address", "china");
        cout << "username: " << username
             << ", password: " << password
             << ", info: " << info
             << ", address: " << address << endl;
    });

    // URL: /has_query?username=peanut&password=
    // 查询参数的值可以为空，所以不能通过判断参数的值是否为空，来判断参数是否存在
    // 而应该通过 has_query() 来判断
    server.GET("/has_query", [](const HttpReq* req, HttpResp* resp) {
        cout << req->query("password") << endl;
        if (req->has_query("password")) {
            cout << "存在参数password" << endl;
        }
        if (req->has_query("info")) {
            cout << "存在参数info" << endl;
        }
    });
    // 4. 启动服务器
    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
