// 任务：获取百度的首页
// 角色：HTTP客户端

#include <iostream>
#include <workflow/WFTaskFactory.h>
#include <workflow/HttpMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/HttpUtil.h> // for HttpHeaderCursor

using namespace std;
using namespace protocol;

// 等待组：原子变量 + 条件变量（等待唤醒的机制）
WFFacilities::WaitGroup waitGroup(1);

void http_callback(WFHttpTask* task)
{   
    // 1. 检查任务的状态
    int state = task->get_state(); // 获取任务的状态
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        waitGroup.done(); // 将waitGroup的值减1
        return;
    }

    // 2. 打印百度的首页
    HttpResponse* resp = task->get_resp();
    // a. 响应行
    cout << resp->get_http_version() << " "
         << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";

    // b. 响应头部
    HttpHeaderCursor cursor(resp);
    string name;
    string value;
    while (cursor.next(name, value)) {
        // 解析成功
        cout << name << ": " << value << "\r\n";
    }

    // c. 响应体
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size);
    cout << static_cast<const char*>(body) << endl;

    waitGroup.done(); // 将waitGroup的值减1
}


int main()
{   
    // 1. 创建HTTP任务
    WFHttpTask* task = WFTaskFactory::create_http_task(
        "http://www.baidu.com",
        3, // redirect_max
        3, // retry_max
        http_callback // 回调函数：当任务执行完成后，才会执行
    );

    // 2. 设置任务参数
    HttpRequest* req = task->get_req();

    req->set_method("GET"); // 默认
    // req->set_request_uri("/");

    // 设置请求头部
    req->set_header_pair("User-Agent", "WorkflowHttpClient");
    req->set_header_pair("Connection", "close"); // 请求结束后，关闭TCP连接

    // 3. 启动任务：交给框架异步执行
    task->start(); // 不阻塞

    // 4. 等待任务执行完毕
    waitGroup.wait(); // 让当前线程阻塞，直到waitGroup的值为0

    return 0;
}