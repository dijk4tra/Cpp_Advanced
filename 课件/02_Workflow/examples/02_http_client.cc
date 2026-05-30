/**
 * [[WFHttpTask 使用示例]]
 *
 * 请求百度的首页，并输出到终端。实现类似 wget <URL> -O - 的效果。
 */

#include <iostream>
#include <string>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h> // for HttpHeaderCursor
#include <workflow/WFFacilities.h>

using namespace std;
using namespace protocol;

// WaitGroup（等待组）：可以看组是条件变量和原子变量的组合。
WFFacilities::WaitGroup waitGroup(1);

void http_callback(WFHttpTask* task)
{
    // 1. 检查任务状态
    int state = task->get_state();
    // 软件设计原则：及早失败原则
    // 疾在腠理，汤熨之所及也；在肌肤，针石之所及也；在肠胃，火齐之所及也；在骨髓，司命之所属
    if (state != WFT_STATE_SUCCESS) {
        // 打印错误信息
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        waitGroup.done(); // 任务结束，将 waitGroup 的值减 1
        return;
    }
    // 2. 任务一定成功，将响应报文输出到终端
    HttpResponse* resp = task->get_resp();
    // 响应行
    cout << resp->get_http_version() << " "
         << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";
    // 响应头部
    HttpHeaderCursor cursor(resp);
    string name;
    string value;
    while (cursor.next(name, value)) {
        cout << name << ": " << value << "\r\n";
    }
    cout << "\r\n";
    // 响应体
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size);
    cout << static_cast<const char*>(body) << endl;
    // 任务结束，将 waitGroup 的值减 1
    waitGroup.done();
}

int main()
{
    // 1. 使用静态工厂方法 WFTaskFactory::create_http_task() 创建Http任务
    // url: 请求的 url
    // redirect_max: 最大的重定向次数
    // retry_max: 最大的重试次数
    // callback: 回调函数，Http任务结束后调用，用于处理服务器的响应
    WFHttpTask* httpTask = WFTaskFactory::create_http_task("http://www.baidu.com", 3, 2, http_callback);
    // 2. 设置 Http 请求
    HttpRequest* req = httpTask->get_req();
    // 设置请求方法
    req->set_method("GET");
    // 设置请求URI，可以不设置，WFHttpTask可以从URL中自动解析。
    req->set_request_uri("/");
    // 添加请求头
    req->add_header_pair("Accept", "*/*");
    req->add_header_pair("User-Agent", "myHttpTask");
    // 修改请求头
    req->set_header_pair("Connection", "close");
    // 3. 将任务交给Workflow异步执行
    httpTask->start();

    // 主线程阻塞，直到 waitGroup 的值减为 0
    waitGroup.wait();
    return 0;
}
