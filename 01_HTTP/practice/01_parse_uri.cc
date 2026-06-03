// 解析URI
// URI: <scheme>://<authority><path>?<query>#<fragment>
// HTTP服务器

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
    // 1. 创建HTTP服务器
    HttpServer server;

    // 2. 注册路由
    server.GET("/*", [](const HttpReq *req, HttpResp *resp)
    {
        // a. 解析请求 URI
        // 在 HTTP 请求中，request_uri 通常是：<path>?<query>
        // 例如：/user/profile?id=100&name=zhangsan
        string uri = req->get_request_uri();

        cout << "========== URI Info ==========" << endl;

        // 原始请求 URI
        cout << "request_uri: " << uri << endl;

        // 当前路由的完整路由规则
        // 这里注册的是 /*，所以 full_path 是 /*
        cout << "full_path: " << req->full_path() << endl;

        // 通配符 * 匹配到的部分
        cout << "match_path: " << req->match_path() << endl;

        // 用户实际请求的 path，不包含 query
        cout << "current_path: " << req->current_path() << endl;

        cout << endl;
        cout << "========== Query Params ==========" << endl;

        // b. 解析查询参数 query
        // 例如 /search?keyword=http&page=1
        // query_list 会解析出：
        // keyword -> http
        // page    -> 1
        const map<string, string> querys = req->query_list();

        if (querys.empty())
        {
            cout << "No query params." << endl;
        }
        else
        {
            for (const auto & [key, value] : querys)
            {
                cout << key << ": " << value << endl;
            }
        }

        cout << endl;
        cout << "========== URI Components ==========" << endl;

        // c. 手动解析 path / query / fragment
        // 注意：/path?query#fragment 服务端通常拿不到 fragment
        string path;
        string query;
        string fragment;

        // 如果找不到，find() 会返回：string::npos
        size_t query_pos = uri.find('?');
        size_t fragment_pos = uri.find('#');

        if (query_pos == string::npos && fragment_pos == string::npos)// 第一种情况：没有 ?，也没有 #
        {   
            path = uri;
        } 
        else if (query_pos != string::npos) // 第二种情况：有 ?
        {
            path = uri.substr(0, query_pos);
            
            if (fragment_pos != string::npos && fragment_pos > query_pos) // 如果找到了 #，并且 # 在 ? 的后面
            {   
                // 从 ? 后面开始，截取到 # 前面
                // substr(pos, len)：从 pos 开始，截取 len 个字符
                query = uri.substr(query_pos + 1, fragment_pos - query_pos - 1);
                // 从 # 后面开始，截取到字符串末尾
                fragment = uri.substr(fragment_pos + 1);
            }
            else // 没有找到 #
            {
                query = uri.substr(query_pos + 1);
            }
        }
        else // 第三种情况：没有 ?，但是有 #
        {
            path = uri.substr(0,fragment_pos);
            fragment = uri.substr(fragment_pos + 1);
        }
        
        cout << "scheme: " << "http" << endl;
        cout << "authority: " << req->header("Host") << endl;
        cout << "host: " << req->header("Host") << endl;
        cout << "path: " << path << endl;
        cout << "query: " << query << endl;
        cout << "fragment: " << fragment << endl;

        cout << "==============================" << endl;

        // d. 生成响应
        resp->String(
            "URI parse success\n"
            "request_uri: " + uri + "\n" +
            "path: " + path + "\n" +
            "query: " + query + "\n" +
            "fragment: " + fragment + "\n"
        );

    });

    // 3. 启动服务器
    if (server.start(8888) == 0)
    {
        cout << "Server started at http://localhost:8888" << endl;
        cout << "Press Enter to stop server..." << endl;
        getchar(); // 按 Enter 键退出
        server.stop();
    }
    else
    {
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}