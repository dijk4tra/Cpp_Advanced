// 任务：获取指定页面的 wget 工具
// 角色：HTTP客户端

#include <cstddef>
#include <iostream>
#include <fstream>
#include <workflow/WFTaskFactory.h>
#include <workflow/HttpMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/HttpUtil.h>

using namespace std;
using namespace protocol;

// 等待组，用于阻塞主线程，直到异步任务完成
WFFacilities::WaitGroup waitGroup(1);

void http_callback(WFHttpTask* task) {
    // 1. 检查任务的状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << "错误：" << WFGlobal::get_error_string(state, task->get_error()) << endl;
        waitGroup.done();
        return;
    }

    // 2. 从任务中取出传递过来的文件名
    const char* filename = static_cast<const char*>(task->user_data);

    ofstream outfile(filename, ios::binary);
    if (!outfile) {
        cerr << "错误：无法打开或创建文件" << filename << endl;
        waitGroup.done();
        return;
    }

    // 3. 从任务中取出响应数据
    HttpResponse* resp = task->get_resp();

    // a. 写入响应行
    outfile << resp->get_http_version() << " "
             << resp->get_status_code() << " "
             << resp->get_reason_phrase() << "\r\n";

    // b. 写入响应头
    HttpHeaderCursor cursor(resp);
    string name;
    string value;
    while (cursor.next(name, value)) {
        outfile << name << ": " << value << "\r\n";
    }

    cout << "\r\n"; // 头部和主体之间的空行

    // c. 写入响应体
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size);
    if (body && size > 0) {
        outfile.write(static_cast<const char*>(body), size);
    }

    outfile.close();
    cout << "成功：响应已全部保存至 " << filename << endl;

    waitGroup.done();
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "用法：" << argv[0] << " <URL> <文件名>" << endl;
        return 1;
    }

    char* url = argv[1];
    char* filename = argv[2];

    WFHttpTask* task = WFTaskFactory::create_http_task(
        url,
        3,
        3,
        http_callback
    );

    task->user_data = filename;

    HttpRequest* req = task->get_req();
    req->set_method("GET");
    req->set_header_pair("User-Agent", "WorkflowHttpClient");
    req->set_header_pair("Connection", "close");

    task->start();
    waitGroup.wait();

    return 0;
}
