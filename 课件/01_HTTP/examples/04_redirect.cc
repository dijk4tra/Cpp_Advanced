/**
 * [[重定向]]
 * 创建一个HTTP服务器，演示不同的重定向状态码的含义。
 *
 * 301: Move Permanently，永久重定向
 * 303: See Other, 跳转到结果页 (GET)
 * 307: Temporary Redirect, 临时重定向 (保持请求方法不变)
 */

#include "wfrest/HttpServer.h"
#include <iostream>

using namespace wfrest;
using namespace std;

int main()
{
    HttpServer server;

    /***************************************************************/
    /*              301: Move Permanently 永久重定向                */
    /***************************************************************/
    server.GET("/status/301", [](const HttpReq* req, HttpResp* resp) {
        // 设置响应状态码
        resp->set_status(301);
        // 添加Location标头，指向新的URL
        resp->add_header_pair("Location", "/newpage/301");
    });

    server.GET("/newpage/301", [](const HttpReq* req, HttpResp* resp) {
        resp->String("<h1>GET newpage/301</h1>");
    });

    /***************************************************************/
    /*         303: See Other 跳转到结果页 (POST -> GET)           */
    /***************************************************************/
    server.POST("/status/303", [](const HttpReq* req, HttpResp* resp) {
        resp->set_status(303);
        resp->add_header_pair("Location", "/newpage/303");
    });

    server.GET("/newpage/303", [](const HttpReq* req, HttpResp* resp) {
        resp->String("<h1>GET newpage/303</h1>");
    });

    server.POST("/newpage/303", [](const HttpReq* req, HttpResp* resp) {
        resp->String("<h1>POST newpage/303</h1>");
    });

    /***************************************************************/
    /*      307: Temporary Redirect 临时重定向 (保持请求方法不变)  */
    /***************************************************************/
    server.GET("/status/307", [](const HttpReq* req, HttpResp* resp) {
        resp->set_status(307);
        resp->add_header_pair("Location", "/newpage/307");
    });

    server.GET("/newpage/307", [](const HttpReq* req, HttpResp* resp) {
        resp->String("<h1>GET newpage/307</h1>");
    });

    server.POST("/status/307", [](const HttpReq* req, HttpResp* resp) {
        resp->set_status(307);
        resp->add_header_pair("Location", "/newpage/307");
    });

    server.POST("/newpage/307", [](const HttpReq* req, HttpResp* resp) {
        resp->String("<h1>POST newpage/307</h1>");
    });

    if (server.start(8888) == 0) {
        getchar(); // 按任意键退出
        server.stop(); // 优雅退出
    } else {
        cerr << "Error: server start failed!" << endl;
        exit(1);
    }
}
