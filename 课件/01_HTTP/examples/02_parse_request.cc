/**
 * [[解析请求报文]]
 *
 * <method> <request-uri> <http-version>
 * <headers>
 *
 * <body>
 */
#include <iostream>
#include <wfrest/HttpServer.h>
#include <workflow/HttpUtil.h>

using namespace std;
using namespace wfrest;
using namespace protocol;

int main()
{
    HttpServer server;

    server.GET("/*", [](const HttpReq* req, HttpResp* resp) {
        // 1. 解析请求行
        cout << req->get_method() << " "
             << req->get_request_uri() << " "
             << req->get_http_version() << "\r\n";
        // 2. 解析请求头部
        // wfrest 没有提供遍历 HTTP 头部的方法，
        // 但我们可以使用 workflow 提供的 HttpHeaderCursor 来遍历。
        HttpHeaderCursor cursor(req);
        string name;
        string value;
        while (cursor.next(name, value)) {
            cout << name << ": " << value << "\r\n";
        }
        cout << "\r\n";
        // GET请求一般是没有主体的
    });

    server.POST("/*", [](const HttpReq* req, HttpResp* resp) {
        // 1. 解析请求行
        cout << req->get_method() << " "
             << req->get_request_uri() << " "
             << req->get_http_version() << "\r\n";
        // 2. 解析请求头部
        // wfrest 没有提供遍历 HTTP 头部的方法，
        // 但我们可以使用 workflow 提供的 HttpHeaderCursor 来遍历。
        HttpHeaderCursor cursor(req);
        string name;
        string value;
        while (cursor.next(name, value)) {
            cout << name << ": " << value << "\r\n";
        }
        cout << "\r\n";
        // 3. 解析主体: HTTP的主体既可以是文本，也可以是二进制数据
        cout << req->body() << endl;
    });

    if (server.start(8888) == 0) {
        getchar();
        server.stop();
    } else {
        cerr << "Error: Server start failed!" << endl;
    }
}
