/**
 * [[URI格式]]
 * <scheme>://<username>:<password>@<host>:<port>/<path>?<query>#<fragment>
 */

#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <wfrest/HttpServer.h>

using namespace std;
using namespace wfrest;

int main()
{
    // 1. 使用默认参数，创建HTTP服务器
    HttpServer server;

    // 2. 注册路由
    server.GET("/*", [](const HttpReq* req, HttpResp* resp) {
        // 获取请求URI
        cout << "uri: " << req->get_request_uri() << endl;
        // 解析路径
        cout << "full_path: " << req->full_path() << endl; // 路由路径
        cout << "match_path: " << req->match_path() << endl; // *号匹配的路径段
        cout << "current_path: " << req->current_path() << endl; // 用户传过来的路径

        // 解析查询参数
        const map<string, string>& querys = req->query_list();
        for (const auto& [name, value] : querys) { // 结构化绑定：C++17
            cout << name << ": " << value << endl;
        }

        // 片段：客户端内部使用，不会发送给服务器
    });

    if (server.start(9527) == 0) { // 监听在通配符地址，端口为9527
        getchar(); // 按任意键退出
        server.stop(); // 优雅退出
    } else {
        cerr << "ERROR: Server start failed!" << endl;
        exit(1);
    }
}
