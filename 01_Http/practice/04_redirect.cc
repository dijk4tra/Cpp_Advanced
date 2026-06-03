// 重定向
// 301 moved permanently: 永久重定向，客户端应将请求中的URL替换为重定向后的URL
// 302 found: 临时定向，客户端应继续使用原始URL发送请求
// 303 see other: 临时重定向，跳转到结果页，第二次请求一定是 GET 方法
// 304 not modified: 资源未修改，客户端可以使用缓存的版本
// 307 temporary redirect: 临时重定向，保持原请求方法不变

// 角色：HTTP服务器

#include <iostream>
#include <wfrest/HttpServer.h>

using namespace std;
using namespace wfrest;

int main(){
    // 1. 创建HTTP服务器
    HttpServer server;

    // 2. 注册路由
    /***************************************************************/
    /*              301: Move Permanently 永久重定向                */
    /***************************************************************/
    server.GET("/status/301", [](const HttpReq* req, HttpResp* resp) {
        // a. 解析请求
        // b. 处理业务
        // c. 生成响应

        // 设置响应状态码
        resp->set_status(301);
        // resp->set_status(HttpStatusMovedPermanently);
        // 添加Location标头，指向新的URL
        resp->set_header_pair("Location", "/newpage/301"); 
    });

    server.GET("/newpage/301", [](const HttpReq* req, HttpResp* resp) {
        resp->String("GET /newpage/301"); // 设置响应体
    });

    /***************************************************************/
    /*         303: See Other 跳转到结果页 (POST -> GET)            */
    /***************************************************************/
    server.POST("/status/303", [](const HttpReq* req, HttpResp* resp) {
        // 设置响应状态码
        resp->set_status(303);
        // resp->set_status(HttpStatusSeeOther);
        // 添加Location标头，指向新的URL
        resp->set_header_pair("Location", "/newpage/303");
    });

    server.GET("/newpage/303", [](const HttpReq* req, HttpResp* resp) {
        resp->String("GET /newpage/303"); // 设置响应体
    });

    server.POST("/newpage/303", [](const HttpReq* req, HttpResp* resp) {
        resp->String("POST /newpage/303"); // 设置响应体
    });
    
    /***************************************************************/
    /*      307: Temporary Redirect 临时重定向 (保持请求方法不变)    */
    /***************************************************************/
    server.GET("status/307", [](const HttpReq* req, HttpResp* resp) {
        resp->set_status(307);
        // resp->set_status(HttpStatusTemporaryRedirect);
        resp->set_header_pair("Location", "/newpage/307");
    });

    server.GET("/newpage/307", [](const HttpReq* req, HttpResp* resp) {
        resp->String("GET /newpage/307"); // 设置响应体
    });

    server.POST("status/307", [](const HttpReq* req, HttpResp* resp) {
        resp->set_status(307);
        // resp->set_status(HttpStatusTemporaryRedirect);
        resp->set_header_pair("Location", "/newpage/307");
    });

    server.POST("/newpage/307", [](const HttpReq* req, HttpResp* resp) {
        resp->String("POST /newpage/307"); // 设置响应体
    });

    // 3. 启动服务器（绑定通配符地址），监听8888端口
    if (server.start(8888) == 0) {
        getchar(); // 按任意键退出
        server.stop(); // 优雅退出
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

}