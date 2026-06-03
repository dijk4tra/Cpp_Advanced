// 角色：客户端
// 任务：解析 HTTP 响应报文
//
// HTTP 响应报文格式：
//
// <HTTP版本号> <状态码> <原因短语>\r\n
// <响应头1>: <值1>\r\n
// <响应头2>: <值2>\r\n
// ...
// \r\n
// <响应体>

#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>

#include <iomanip>
#include <iostream>
#include <string>

using namespace std;
using namespace protocol;

// 用 WaitGroup 等待异步任务结束
static WFFacilities::WaitGroup wait_group(1);

// 打印响应体的二进制内容
void print_body_as_hex(const void *body, size_t size) 
{
    const unsigned char *p = static_cast<const unsigned char *>(body);

    cout << "body hex: ";

    for (size_t i = 0; i < size; ++i) {
    cout << hex << setw(2) << setfill('0') << static_cast<int>(p[i]) << " ";
    }
    cout << dec << endl;
}

// HTTP 任务完成后，workflow 会自动调用这个回调函数
void http_callback(WFHttpTask *task) {
    // 1. 判断任务是否成功
    int state = task->get_state();

    if (state != WFT_STATE_SUCCESS) {
        // 打印错误信息
        cout << "HTTP task failed: "
             << WFGlobal::get_error_string(state, task->get_error()) << endl;

      // 通知主线程：任务已经结束
      wait_group.done();
      return;
    }

    // 2. 任务成功，说明已经收到 HTTP 响应
    HttpResponse *resp = task->get_resp();

    cout << "========== HTTP Response ==========" << endl;

    // a. 解析响应行 Status Line
    // HTTP 响应报文第一行叫做响应行。
    // 格式：<HTTP版本号> <状态码> <原因短语>
    cout << "========== Status Line ==========" << endl;

    cout << resp->get_http_version() << " " << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";

    cout << endl;

    cout << "http version: " << resp->get_http_version() << endl;
    cout << "status code: " << resp->get_status_code() << endl;
    cout << "reason phrase: " << resp->get_reason_phrase() << endl;

    cout << endl;

    // b. 解析响应头 Headers
    // 响应头位于响应行之后。
    // 格式：Header-Name: Header-Value
    cout << "========== Headers ==========" << endl;
    HttpHeaderCursor cursor(resp);

    string name;
    string value;

    while (cursor.next(name, value)) {
        cout << name << ": " << value << "\r\n";
    }

    // 响应头和响应体之间有一个空行
    cout << "\r\n";

    // c. 解析响应体 Body
    // 响应体在响应头后面的空行之后
    cout << "========== Body ==========" << endl;

    const void *body;
    size_t size;

    resp->get_parsed_body(&body, &size);

    cout << "body size: " << size << " bytes" << endl;

    if (body == nullptr || size == 0) {
        cout << "empty body" << endl;
    } else {
        // 如果响应体是 HTML、JSON、普通文本，可以这样打印：
        // 注意：不要直接用 cout << static_cast<const char *>(body)
        // 因为 body 不一定以 '\0' 结尾
        // 正确方式是用 body + size 构造 string
        string body_text(static_cast<const char *>(body), size);

        cout << "body as text:" << endl;
        cout << body_text << endl;

        cout << endl;

        // 如果响应体是二进制，可以按十六进制查看
        // cout << "body as binary:" << endl;
        // print_body_as_hex(body, size);
    }

    cout << "===================================" << endl;

    // 3. 通知主线程：HTTP 任务已经完成
    wait_group.done();
}

int main() {
    // 1. 创建 HTTP 客户端任务
    WFHttpTask *task = WFTaskFactory::create_http_task(
        "http://stu.cskaoyan.com/", // URL，表示要访问的资源
        3,                      // redirect_max，最大重定向次数
        3,            // retry_max，请求失败后的最大重试次数
        http_callback // 回调函数，任务完成后自动调用
    );

    // 2. 设置 HTTP 请求
    // 拿到客户端要发送的 HTTP 请求对象
    HttpRequest *req = task->get_req();

    req->set_method("GET"); // 设置请求方法
    req->set_request_uri("/"); // 设置请求 URI
    // 也可以设置一些请求头
    req->add_header_pair("User-Agent", "workflow-http-client");
    req->add_header_pair("Accept", "*/*");
    req->add_header_pair("Connection", "close");

    // 3. 启动异步任务
    task->start();

    cout << "任务已提交，等待 HTTP 响应..." << endl;

    // 4. 阻塞主线程，等待回调函数执行完成
    wait_group.wait();

    cout << "主线程结束。" << endl;

    return 0;
}
