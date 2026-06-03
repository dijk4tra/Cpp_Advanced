// URI解析示例
// URI格式：<scheme>://<authority><path>?<query>#<fragment>
// 本示例演示HTTP服务器中如何解析请求URI、路径和查询参数

#include <cstdio>
#include <iostream>
#include <wfrest/HttpServer.h>

using namespace std;
using namespace wfrest;

int main() {
    // 1. 创建HTTP服务器对象
    HttpServer server;

    // 2. 注册GET路由
    // "/*" 表示匹配所有路径
    server.GET("/*", [](const HttpReq *req, HttpResp *resp) {
        // a. 解析请求URI
        // 请求URI通常包含：<path>?<query>
        cout << req->get_request_uri() << endl;

        // 解析路径相关信息
        cout << "full_path: " << req->full_path() << endl;       // 路由注册时的完整路径，例如：/*
        cout << "match_path: " << req->match_path() << endl;     // 通配符 * 实际匹配到的路径部分
        cout << "current_path: " << req->current_path() << endl; // 客户端实际请求的路径

        // 解析查询参数，即URI中 ? 后面的 key=value 部分
        const map<string, string> querys = req->query_list();

        // 普通写法：基于范围的for循环
        // for (const auto& p : querys) {
        //     string key = p.first;
        //     string value = p.second;
        //     cout << key << ": " << value << endl; 
        // }

        // C++17写法：结构化绑定，直接拆解key和value
        for (const auto& [key, value] : querys) {
            cout << key << ": " << value << endl; 
        }

        // b. 处理业务逻辑

        // c. 生成响应
        // 当前示例仅在服务端打印信息，未向客户端返回响应内容
        cout << "I love 33" << endl;
    });

    // 3. 启动服务器，监听8888端口
    if (server.start(8888) == 0) { // start() 启动成功后不会阻塞主线程
        getchar();                 // 阻塞主线程，按Enter后继续执行
        server.stop();             // 停止服务器，使程序有序退出
    } else {
        // 服务器启动失败
        cerr << "Error: server start FAILED!" << endl;
        exit(1);
    }
}