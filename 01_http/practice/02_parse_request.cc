// 角色：服务器
// 任务：解析 HTTP 请求报文
//
// HTTP 请求报文格式大致如下：
//
// <请求方法> <URI> <HTTP版本号>\r\n
// <请求头1>: <值1>\r\n
// <请求头2>: <值2>\r\n
// ...
// \r\n
// <请求体>
//
// 例如：
//
// POST /user/profile?id=10&name=yuwenjia HTTP/1.1
// Host: localhost:8888
// Content-Type: application/json
// Content-Length: 23
//
// {"age":23,"city":"BJ"}

#include <wfrest/HttpServer.h>
#include <workflow/HttpUtil.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <string>

using namespace std;
using namespace protocol;
using namespace wfrest;

// 打印二进制请求体：把每个字节转换成十六进制打印出来
// 因为请求体不一定是普通文本，也可能是图片、压缩包、音频等二进制数据
void print_binary_body(const string& body)
{
    // body.size() 表示请求体的真实字节数
    cout << "Body size: " << body.size() << " bytes" << endl;
    cout << "Body hex: ";
    
    // unsigned char 表示按无符号字节处理
    // 避免 char 被当成负数
    for (unsigned char ch : body)
    {
        cout << hex                   // hex：以十六进制输出
             << setw(2)               // setw(2)：每个字节至少占 2 位
             << setfill('0')          // setfill('0')：不足 2 位时前面补 0
             << static_cast<int>(ch)  // static_cast<int>(ch)：把字节转换成整数再打印
             << " ";
    }

    // 打印完十六进制之后，恢复成十进制输出
    // 否则后面打印数字时也会继续使用十六进制
    cout << dec << endl;
}


int main()
{
    // 1. 创建HTTP服务器
    HttpServer server;

    // 2. 注册POST路由
    server.POST("/*", [](const HttpReq *req, HttpResp *resp){
        cout << "========== HTTP Request ==========" << endl;

        // a. 解析请求行 Request Line
        // 格式：<请求方法> <URI> <HTTP版本号>
        cout << "========== Request Line ==========" << endl;
        // 按照 HTTP 请求行格式打印
        cout << req->get_method() << " "
             << req->get_request_uri() << " "
             << req->get_http_version() << "\r\n";
        
        cout << endl;

        // 分别打印请求行中的三个部分
        cout << "method: " << req->get_method() << endl;
        cout << "uri: " << req->get_request_uri() << endl;
        cout << "http version: " << req->get_http_version() << endl;

        cout << endl;

        // b. 解析请求头 Headers
        // 格式：Header-Name: Header-Value
        cout << "========== Headers ==========" << endl;
        HttpHeaderCursor cursor(req);

        string name;
        string value;

        while(cursor.next(name, value))
        {
            cout << name << ": " << value << "\r\n";
        }

        // 请求头和请求体之间有一个空行
        cout << endl;

        // c. 解析 URI 相关信息
        cout << "========== URI Info ==========" << endl;

        cout << "request_uri: " << req->get_request_uri() << endl;
        cout << "full_path: " << req->full_path() << endl;
        cout << "match_path: " << req->match_path() << endl;
        cout << "current_path: " << req->current_path() << endl;

        cout << endl;

        // d. 解析查询参数 Query String
        // 查询参数就是 URI 中 ? 后面的部分
        cout << "========== Query Params ==========" << endl;
        
        const map<string, string> querys = req->query_list();

        if(querys.empty())
        {
            cout << "no query params" << endl;
        }
        else
        {
            // C++17 结构化绑定
            for (const auto& [key, value] : querys)
            {
                cout << key << ": " << value << endl;
            }
        }

        cout << endl;

        // e. 解析请求体 Body
        // 请求体位于请求头后面的空行之后
        cout << "========== Body ==========" << endl;
        
        string body = req->body();

        // 如果请求体是 JSON、普通文本，可以直接打印
        // 如果请求体是二进制数据，直接打印可能会乱码
        cout << "body as text:" << endl;
        cout << body << endl;

        cout << endl;

        // f. 按二进制方式查看请求体
        cout << "body as binary:" << endl;
        print_binary_body(body);

        cout << "==================================" << endl;

        // g. 生成响应 Response
        // 服务端解析完请求后，需要给客户端返回响应
        // resp->String(...) 表示返回一个字符串响应体
        string resp_body;

        resp_body += "HTTP request parse success\n";
        resp_body += "method: " + string(req->get_method()) + "\n";
        resp_body += "uri: " + string(req->get_request_uri()) + "\n";
        resp_body += "http version: " + string(req->get_http_version()) + "\n";
        resp_body += "current path: " + string(req->current_path()) + "\n";
        resp_body += "body size: " + to_string(body.size()) + " bytes\n";

        resp->String(resp_body);

    });

    // 3. 启动服务器
    if (server.start(8888) == 0)
    {   
        cout << "Server started at http://localhost:8888" << endl;
        cout << "Press Enter to stop server..." << endl;
        getchar(); // 按Enter退出
        server.stop();
    }
    else
    {
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}