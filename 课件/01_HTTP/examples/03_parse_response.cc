/**
 * [[响应报文格式]]
 * <http-version> <status-code> <reason-phrase>
 * <headers>
 *
 * <body>
 */

#include <iostream>
#include <workflow/HttpUtil.h>
#include <workflow/WFTaskFactory.h>

using namespace protocol;
using namespace std;

// 回调函数：HTTP任务执行完后会调用这个函数
// 我们在这个函数中解析响应报文
void http_callback(WFHttpTask* task)
{
    // 1. 检查任务状态 (及早失败原则)
    // 如果任务执行失败，打印错误信息并退出
    if (task->get_state() != WFT_STATE_SUCCESS) {
        cerr << "任务执行失败!" << endl;
        exit(1);
    }
    // 2. 解析响应行
    HttpResponse* resp = task->get_resp();
    cout << resp->get_http_version() << " "
         << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";
    // 3. 解析响应头部
    HttpHeaderCursor cursor { resp };
    string name;
    string value;
    while (cursor.next(name, value)) {
        cout << name << ": " << value << "\r\n";
    }
    cout << "\r\n";
    // 4. 解析响应主体
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size); /* 不会复制数据 */
    cout << "body size: " << size << endl;
    // 注意：如果响应主体是二进制数据，直接输出会出现乱码
    // 这里假设响应主体是文本数据
    cout << static_cast<const char*>(body) << endl;
}

int main()
{
    // 1. 创建一个HTTP任务
    WFHttpTask* task = WFTaskFactory::create_http_task(
        "http://www.baidu.com", /* request-uri */
        3, /* redirect-max */
        3, /* retry_max */
        http_callback /* 回调函数：当任务执行完后，才会执行 */
    );
    // 2. 设置HTTP任务：构建请求内容
    HttpRequest* req = task->get_req();
    req->set_method("GET");
    req->set_request_uri("/");
    // 3. 将任务交给 workflow 框架异步执行!
    task->start();
    // 4. 主线程等待
    getchar(); // 按任意键终止程序
    return 0;
}
