// 解析 URI
// URI 的完整格式：
// <scheme>://<authority><path>?<query>#<fragment>
// <方案>://<主机信息><路径>?<查询参数>#<片段>
//
// 例如：
// http://localhost:8888/user/profile?id=10&name=yuwenjia#title
//
// 可以拆成：
// scheme    = http
// authority = localhost:8888
// path      = /user/profile
// query     = id=10&name=yuwenjia
// fragment  = title
//
// 注意：
// 在 HTTP 请求发送到服务器时，浏览器通常不会把 #fragment 发给服务器。
// 也就是说，服务端一般只能拿到：
//
// /user/profile?id=10&name=yuwenjia
//
// 而拿不到：
//
// #title

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include <wfrest/HttpServer.h>

using namespace std;
using namespace wfrest;

int main()
{
    // 1. 创建 HTTP 服务器
    //
    // HttpServer 是 wfrest 提供的 HTTP 服务器类。
    // 后面可以通过 server.GET、server.POST 等注册路由。
    HttpServer server;


    // 2. 注册 GET 路由
    //
    // "/*" 表示匹配所有 GET 请求路径。
    //
    // 例如下面这些请求都可以匹配：
    //
    // GET /
    // GET /user
    // GET /user/profile
    // GET /user/profile?id=10
    //
    // 回调函数参数：
    //
    // const HttpReq *req
    // 表示客户端发来的 HTTP 请求。
    // 一般只读取请求内容，所以使用 const。
    //
    // HttpResp *resp
    // 表示服务器要返回给客户端的 HTTP 响应。
    // 需要修改响应内容，所以不能加 const。
    server.GET("/*", [](const HttpReq *req, HttpResp *resp)
    {
        // ============================================================
        // a. 获取原始请求 URI
        // ============================================================
        //
        // 在 HTTP 请求中，request_uri 通常是：
        //
        // <path>?<query>
        //
        // 例如：
        //
        // /user/profile?id=100&name=zhangsan
        //
        // 注意：
        // 这里通常不包含 scheme、authority、fragment。
        //
        // 也就是说，服务端拿到的一般不是：
        //
        // http://localhost:8888/user/profile?id=100#title
        //
        // 而是：
        //
        // /user/profile?id=100

        string uri = req->get_request_uri();

        cout << "========== URI Info ==========" << endl;

        // 原始请求 URI
        //
        // 例如：
        //
        // /user/profile?id=10&name=yuwenjia
        cout << "request_uri: " << uri << endl;


        // 当前匹配到的完整路由规则
        //
        // 因为这里注册的是：
        //
        // server.GET("/*", ...)
        //
        // 所以 full_path() 一般输出：
        //
        // /*
        //
        // 它表示“服务器定义的路由模板”。
        cout << "full_path: " << req->full_path() << endl;


        // 通配符 * 匹配到的部分
        //
        // 例如：
        //
        // 路由规则：/*
        // 请求路径：/user/profile
        //
        // 那么 * 匹配到：
        //
        // user/profile
        //
        // 注意：match_path() 前面通常没有 /。
        cout << "match_path: " << req->match_path() << endl;


        // 用户实际请求的 path，不包含 query 参数
        //
        // 例如：
        //
        // 请求 URI：
        //
        // /user/profile?id=10&name=yuwenjia
        //
        // current_path() 得到：
        //
        // /user/profile
        cout << "current_path: " << req->current_path() << endl;

        cout << endl;


        // ============================================================
        // b. 解析查询参数 query
        // ============================================================
        //
        // query 是 URI 中 ? 后面的部分。
        //
        // 例如：
        //
        // /search?keyword=http&page=1
        //
        // query 部分是：
        //
        // keyword=http&page=1
        //
        // wfrest 的 query_list() 会把它解析成 map：
        //
        // keyword -> http
        // page    -> 1

        cout << "========== Query Params ==========" << endl;

        const map<string, string> querys = req->query_list();

        if (querys.empty())
        {
            cout << "No query params." << endl;
        }
        else
        {
            // C++17 结构化绑定
            //
            // key   表示参数名
            // value 表示参数值
            //
            // 例如：
            //
            // id=10&name=yuwenjia
            //
            // 会输出：
            //
            // id: 10
            // name: yuwenjia
            for (const auto& [key, value] : querys)
            {
                cout << key << ": " << value << endl;
            }
        }

        cout << endl;


        // ============================================================
        // c. 手动解析 path / query / fragment
        // ============================================================
        //
        // URI 简化结构：
        //
        // /path?query#fragment
        //
        // 例如：
        //
        // /user/profile?id=10&name=yuwenjia#title
        //
        // 拆分结果：
        //
        // path     = /user/profile
        // query    = id=10&name=yuwenjia
        // fragment = title
        //
        // 但是注意：
        // 服务端通常拿不到 fragment。
        // 下面的代码只是为了演示 URI 的完整解析逻辑。

        string path;
        string query;
        string fragment;


        // find('?') 用来查找 ? 第一次出现的位置。
        //
        // 如果找到了，返回 ? 的下标。
        // 如果没找到，返回 string::npos。
        size_t query_pos = uri.find('?');


        // find('#') 用来查找 # 第一次出现的位置。
        //
        // 如果找到了，返回 # 的下标。
        // 如果没找到，返回 string::npos。
        size_t fragment_pos = uri.find('#');


        // ------------------------------------------------------------
        // 情况 1：没有 ?，也没有 #
        // ------------------------------------------------------------
        //
        // 例如：
        //
        // /user/profile
        //
        // 整个 uri 都是 path。
        //
        // 结果：
        //
        // path     = /user/profile
        // query    = 空字符串
        // fragment = 空字符串

        if (query_pos == string::npos && fragment_pos == string::npos)
        {
            path = uri;
        }


        // ------------------------------------------------------------
        // 情况 2：有 ?
        // ------------------------------------------------------------
        //
        // 例如：
        //
        // /user/profile?id=10&name=yuwenjia
        //
        // 或者：
        //
        // /user/profile?id=10#title
        //
        // ? 前面的部分是 path。
        // ? 后面的部分是 query。
        //
        // 如果后面还有 #，那么 ? 和 # 中间是 query，# 后面是 fragment。

        else if (query_pos != string::npos)
        {
            // substr(0, query_pos)
            //
            // 表示从下标 0 开始，截取 query_pos 个字符。
            //
            // 也就是截取 ? 前面的内容。
            //
            // 例如：
            //
            // uri = /user/profile?id=10
            //
            // path = /user/profile
            path = uri.substr(0, query_pos);


            // 如果找到了 #，并且 # 在 ? 的后面，
            // 说明 URI 结构是：
            //
            // /path?query#fragment
            //
            // 例如：
            //
            // /user/profile?id=10#title
            if (fragment_pos != string::npos && fragment_pos > query_pos)
            {
                // query_pos + 1
                //
                // 表示从 ? 后面一个字符开始。
                //
                // fragment_pos - query_pos - 1
                //
                // 表示 ? 和 # 中间的字符数量。
                //
                // 所以这一句的含义是：
                //
                // 从 ? 后面开始，截取到 # 前面。
                query = uri.substr(query_pos + 1,
                                   fragment_pos - query_pos - 1);


                // fragment_pos + 1
                //
                // 表示从 # 后面一个字符开始，
                // 一直截取到字符串末尾。
                fragment = uri.substr(fragment_pos + 1);
            }
            else
            {
                // 有 ?，但是没有 #
                //
                // 例如：
                //
                // /user/profile?id=10&name=yuwenjia
                //
                // query 就是 ? 后面的所有内容。
                query = uri.substr(query_pos + 1);
            }
        }


        // ------------------------------------------------------------
        // 情况 3：没有 ?，但是有 #
        // ------------------------------------------------------------
        //
        // 例如：
        //
        // /user/profile#title
        //
        // # 前面的部分是 path。
        // # 后面的部分是 fragment。
        //
        // 注意：
        // 这种情况在真实 HTTP 服务端中通常看不到，
        // 因为浏览器一般不会把 #fragment 发给服务器。

        else
        {
            path = uri.substr(0, fragment_pos);
            fragment = uri.substr(fragment_pos + 1);
        }


        // ============================================================
        // d. 输出 URI 各个组件
        // ============================================================

        cout << "========== URI Components ==========" << endl;


        // scheme 表示协议。
        //
        // 例如：
        //
        // http
        // https
        //
        // 这里服务器监听的是普通 HTTP，所以直接写 http。
        //
        // 如果使用 HTTPS，则应该是 https。
        cout << "scheme: " << "http" << endl;


        // authority 表示权威部分。
        //
        // URL 中的 authority 通常是：
        //
        // host:port
        //
        // 例如：
        //
        // localhost:8888
        //
        // 在 HTTP 请求中，它通常来自请求头 Host。
        cout << "authority: " << req->header("Host") << endl;


        // host 表示主机。
        //
        // 这里为了演示，直接使用 Host 请求头。
        //
        // 注意：
        // Host 请求头可能包含端口号。
        //
        // 例如：
        //
        // localhost:8888
        //
        // 严格来说：
        //
        // host = localhost
        // port = 8888
        //
        // 这里暂时没有继续拆分 host 和 port。
        cout << "host: " << req->header("Host") << endl;


        // 手动解析出来的 path
        cout << "path: " << path << endl;


        // 手动解析出来的 query
        cout << "query: " << query << endl;


        // 手动解析出来的 fragment
        //
        // 真实 HTTP 服务端一般拿不到 fragment，
        // 所以这里大多数情况下是空字符串。
        cout << "fragment: " << fragment << endl;

        cout << "==============================" << endl;


        // ============================================================
        // e. 生成响应
        // ============================================================
        //
        // resp->String(...) 表示给客户端返回字符串响应。
        //
        // 这里把解析出来的结果返回给浏览器或 Postman，
        // 方便你直接看到服务器解析结果。

        string body;

        body += "URI parse success\n";
        body += "request_uri: " + uri + "\n";
        body += "scheme: http\n";
        body += "authority: " + string(req->header("Host")) + "\n";
        body += "host: " + string(req->header("Host")) + "\n";
        body += "path: " + path + "\n";
        body += "query: " + query + "\n";
        body += "fragment: " + fragment + "\n";

        resp->String(body);
    });


    // 3. 启动服务器
    //
    // server.start(8888)
    //
    // 表示监听 8888 端口。
    //
    // 如果返回 0，表示启动成功。
    // 如果返回非 0，表示启动失败。
    //
    // 注意：
    // server.start() 启动服务器后不会阻塞主线程。
    // 所以下面使用 getchar() 阻塞程序，
    // 让服务器保持运行状态。

    if (server.start(8888) == 0)
    {
        cout << "Server started at http://localhost:8888" << endl;
        cout << "Press Enter to stop server..." << endl;

        // 按 Enter 键后，程序继续执行
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