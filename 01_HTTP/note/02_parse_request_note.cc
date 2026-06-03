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
#include <map>

using namespace std;
using namespace protocol;
using namespace wfrest;


// 打印二进制请求体
//
// 为什么需要这个函数？
// 因为请求体不一定是普通文本，也可能是图片、压缩包、音频等二进制数据。
// 二进制数据中可能包含不可见字符，直接 cout 可能看不清楚。
// 所以这里把每个字节转换成十六进制打印出来。
void print_binary_body(const string& body)
{
    // body.size() 表示请求体的真实字节数
    // 注意：不要用 strlen(body.c_str()) 来计算二进制数据长度
    // 因为二进制中可能包含 '\0'，strlen 遇到 '\0' 会提前停止
    cout << "Body size: " << body.size() << " bytes" << endl;

    cout << "Body hex: ";

    // unsigned char 表示按无符号字节处理
    // 避免 char 被当成负数
    for (unsigned char ch : body) {
        // hex：以十六进制输出
        // setw(2)：每个字节至少占 2 位
        // setfill('0')：不足 2 位时前面补 0
        // static_cast<int>(ch)：把字节转换成整数再打印
        cout << hex
             << setw(2)
             << setfill('0')
             << static_cast<int>(ch)
             << " ";
    }

    // 打印完十六进制之后，恢复成十进制输出
    // 否则后面打印数字时也会继续使用十六进制
    cout << dec << endl;
}


int main()
{
    // 1. 创建 HTTP 服务器对象
    //
    // HttpServer 是 wfrest 提供的 HTTP 服务器类。
    // 后面可以通过 server.GET、server.POST 等注册不同方法的路由。
    HttpServer server;


    // 2. 注册 POST 路由
    //
    // "/*" 表示匹配所有 POST 请求路径。
    //
    // 例如这些请求都可以匹配：
    // POST /
    // POST /login
    // POST /user/profile
    // POST /upload/file
    //
    // 回调函数参数：
    //
    // const HttpReq *req
    // 表示客户端发来的 HTTP 请求。
    // 一般只读取它的内容，所以用 const。
    //
    // HttpResp *resp
    // 表示服务器要返回给客户端的 HTTP 响应。
    // 需要修改响应内容，所以不能加 const。
    server.POST("/*", [](const HttpReq *req, HttpResp *resp) {
        cout << "========== HTTP Request ==========" << endl;


        // ============================================================
        // a. 解析请求行 Request Line
        // ============================================================
        //
        // HTTP 请求报文第一行叫做“请求行”。
        //
        // 格式：
        //
        // <请求方法> <URI> <HTTP版本号>
        //
        // 例如：
        //
        // POST /user/profile?id=10&name=yuwenjia HTTP/1.1
        //
        // 其中：
        //
        // 请求方法      POST
        // URI           /user/profile?id=10&name=yuwenjia
        // HTTP版本号    HTTP/1.1

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


        // ============================================================
        // b. 解析请求头 Headers
        // ============================================================
        //
        // 请求头位于请求行后面。
        //
        // 格式：
        //
        // Header-Name: Header-Value
        //
        // 例如：
        //
        // Host: localhost:8888
        // Content-Type: application/json
        // Content-Length: 30
        // User-Agent: curl/7.81.0
        //
        // HttpHeaderCursor 用来遍历所有 HTTP 请求头。

        cout << "========== Headers ==========" << endl;

        HttpHeaderCursor cursor(req);

        string name;
        string value;

        // cursor.next(name, value)
        //
        // 如果还有下一个请求头，就返回 true，
        // 并把请求头名字放到 name 中，把请求头的值放到 value 中。
        //
        // 如果没有更多请求头，就返回 false，循环结束。
        while (cursor.next(name, value)) {
            cout << name << ": " << value << "\r\n";
        }

        // 请求头和请求体之间有一个空行
        cout << endl;


        // ============================================================
        // c. 解析 URI 相关信息
        // ============================================================
        //
        // URI 是请求行中的第二部分。
        //
        // 例如：
        //
        // /user/profile?id=10&name=yuwenjia
        //
        // 在 wfrest 中，可以通过下面几个接口查看路径相关信息：
        //
        // get_request_uri()
        // 原始请求 URI，通常包含 path 和 query。
        //
        // full_path()
        // 当前匹配到的路由规则。
        // 因为我们注册的是 "/*"，所以这里通常是 "/*"。
        //
        // match_path()
        // 通配符 * 匹配到的部分。
        // 例如请求 /user/profile，路由是 /*，
        // 那么 * 匹配到 user/profile。
        //
        // current_path()
        // 用户实际请求的路径，不包含 query 参数。
        // 例如 /user/profile?id=10，
        // current_path() 是 /user/profile。

        cout << "========== URI Info ==========" << endl;

        cout << "request_uri: " << req->get_request_uri() << endl;
        cout << "full_path: " << req->full_path() << endl;
        cout << "match_path: " << req->match_path() << endl;
        cout << "current_path: " << req->current_path() << endl;

        cout << endl;


        // ============================================================
        // d. 解析查询参数 Query String
        // ============================================================
        //
        // 查询参数就是 URI 中 ? 后面的部分。
        //
        // 例如：
        //
        // /user/profile?id=10&name=yuwenjia&age=23
        //
        // 查询参数是：
        //
        // id=10&name=yuwenjia&age=23
        //
        // wfrest 的 query_list() 会把查询参数解析成 map。
        //
        // 解析结果大致是：
        //
        // id   -> 10
        // name -> yuwenjia
        // age  -> 23

        cout << "========== Query Params ==========" << endl;

        const map<string, string> querys = req->query_list();

        if (querys.empty()) {
            cout << "no query params" << endl;
        } else {
            // C++17 结构化绑定
            //
            // key 是参数名
            // value 是参数值
            for (const auto& [key, value] : querys) {
                cout << key << ": " << value << endl;
            }
        }

        cout << endl;


        // ============================================================
        // e. 解析请求体 Body
        // ============================================================
        //
        // 请求体位于请求头后面的空行之后。
        //
        // 常见请求体类型：
        //
        // 1. 普通文本
        // 2. JSON
        // 3. 表单数据
        // 4. 文件内容
        // 5. 图片、音频、压缩包等二进制数据
        //
        // req->body() 可以拿到请求体内容。
        //
        // 注意：
        // std::string 不只可以保存文本，也可以保存二进制数据。
        // 所以即使请求体是二进制，也可以先用 string 接收。

        cout << "========== Body ==========" << endl;

        string body = req->body();

        // 如果请求体是 JSON、普通文本，可以直接打印。
        //
        // 例如：
        //
        // {"age":23,"city":"BJ"}
        //
        // 但是如果请求体是图片、压缩包等二进制数据，
        // 直接 cout 可能显示乱码，甚至看起来像什么都没输出。
        cout << "body as text:" << endl;
        cout << body << endl;

        cout << endl;


        // ============================================================
        // f. 按二进制方式查看请求体
        // ============================================================
        //
        // 对于二进制请求体，更推荐：
        //
        // 1. 看 body.size()
        // 2. 按字节打印十六进制
        //
        // 这样可以避免不可见字符、乱码、'\0' 等问题。

        cout << "body as binary:" << endl;
        print_binary_body(body);

        cout << "==================================" << endl;


        // ============================================================
        // g. 生成响应 Response
        // ============================================================
        //
        // 服务端解析完请求后，需要给客户端返回响应。
        //
        // resp->String(...) 表示返回一个字符串响应体。
        //
        // 这里我们把解析出来的一些关键信息返回给客户端，
        // 方便在浏览器或者 curl 中查看结果。

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
    //
    // server.start(8888) 表示监听 8888 端口。
    //
    // 如果返回 0，表示启动成功。
    // 如果返回非 0，表示启动失败。
    //
    // 注意：
    // server.start() 启动服务器后不会阻塞主线程，
    // 所以下面用 getchar() 阻塞程序，
    // 让服务器保持运行状态。
    if (server.start(8888) == 0)
    {
        cout << "Server started at http://localhost:8888" << endl;
        cout << "Press Enter to stop server..." << endl;

        // 按回车后，程序继续向下执行
        getchar();

        // 停止服务器
        server.stop();
    }
    else
    {
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}