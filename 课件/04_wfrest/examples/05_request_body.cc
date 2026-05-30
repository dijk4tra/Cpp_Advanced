#include <iostream>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer server;

    server.POST("/request_body", [](const HttpReq* req, HttpResp* resp) {
        // 只是单纯地获取请求体
        // 不会解析x-www-form-urlencoded表单
        // 不会解析form-data表单
        string& body = req->body();
        cout << body << endl;
    });

    // curl -v http://ip:port/form-urlencoded \
    // -H "Content-Type: application/x-www-form-urlencoded" \
    // -d 'user=admin&password=123456'
    server.POST("/form-urlencoded", [](const HttpReq* req, HttpResp* resp) {
        // 校验请求体的类型：application/x-www-form-urlencoded
        if (req->content_type() != APPLICATION_URLENCODED) {
            resp->set_status(HttpStatusBadRequest); // 响应状态码: 400 BadRequest
            return;
        }
        // req->form_kv() 获取表单数据
        map<string, string>& form = req->form_kv();
        for (const auto& [key, value] : form) {
            cout << key << ": " << value << endl;
        }
    });

    // curl -X POST http://ip:port/form-data \
    // -F "file=@/path/file" \
    // -H "Content-Type: multipart/form-data"
    server.POST("/form-data", [](const HttpReq* req, HttpResp* resp) {
        // 校验请求体的类型：multipart/form-data
        if (req->content_type() != MULTIPART_FORM_DATA) {
            resp->set_status(HttpStatusBadRequest); // 响应状态码: 400 BadRequest
            return;
        }
        /*
         * https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods/POST
         * <key, <filename, content>>
         * using Form = map<string, pair<string, string>>;
         */
        const Form& formData = req->form();
        for (const auto& [key, file] : formData) {
            cout << "key: " << key << endl;
            cout << "filename: " << file.first << endl;
            cout << "content: " << file.second << endl;
        }
    });

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
