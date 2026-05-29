// 角色：服务器
// 任务：解析HTTP请求报文
// HTTP请求报文格式：
// <请求方法> <URI> <HTTP版本号>\r\n
// <请求头1>: <值1>\r\n
// <请求头2>: <值2>\r\n
// ...
// \r\n
// <请求体>

#include <wfrest/HttpServer.h>
#include <iostream>
#include <workflow/HttpUtil.h> // HTTP工具类，例如HttpHeaderCursor

using namespace std;
using namespace protocol;
using namespace wfrest;

int main() 
{
    // 1. 创建HTTP服务器对象
    HttpServer server;

    // 2. 注册POST路由
    // "/*" 表示匹配所有POST请求路径
    server.POST("/*", [](const HttpReq *req, HttpResp *resp) {
        // a. 解析请求

        // 获取请求URI
        cout << req->get_request_uri() << endl;

        // 解析请求行：
        // <请求方法> <URI> <HTTP版本号>\r\n
        cout << req->get_method() << " "
             << req->get_request_uri() << " "
             << req->get_http_version() << "\r\n";

        // 解析请求头：
        // <请求头名>: <请求头值>\r\n
        HttpHeaderCursor cursor(req);
        string name;
        string value;
        while (cursor.next(name, value)) {
            cout << name << ": " << value << "\r\n";
        }

        // 空行表示请求头结束
        cout << "\r\n";

        // 解析请求体
        // body() 返回请求体内容，适合文本数据
        // 若请求体是二进制数据，应按字节流处理，避免直接当作字符串打印
        cout << req->body() << endl;

        // b. 处理业务逻辑

        // c. 生成响应
    });
    
    // 3. 启动服务器，监听8888端口
    if (server.start(8888) == 0)
    {   
        getchar();    // 阻塞主线程，按Enter后退出
        server.stop(); // 停止服务器，使程序有序退出
    } else {
        // 服务器启动失败
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }
}