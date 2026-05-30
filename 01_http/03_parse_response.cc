// 角色：HTTP 客户端
// 任务：发送 HTTP 请求，并解析服务器返回的 HTTP 响应报文
//
// HTTP 响应报文格式大致如下：
//
// <HTTP版本号> <状态码> <原因短语>\r\n
// <响应头1>: <值1>\r\n
// <响应头2>: <值2>\r\n
// ...
// \r\n
// <响应体>
//
// 例如：
//
// HTTP/1.1 200 OK
// Content-Type: text/html
// Content-Length: 1024
//
// <html>...</html>

#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTaskFactory.h>

using namespace std;
using namespace protocol;

void http_callback(WFHttpTask* task)
{
    // 1. 判断 HTTP 任务是否执行成功
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS)
    {   
        // 打印任务失败原因
        cout << WFGlobal::get_error_string(state, task->get_error()) << endl;
        return;
    }

    // 2. 任务执行成功，说明已经收到服务器返回的 HTTP 响应
    HttpResponse* resp = task->get_resp();

    // 3. 打印响应行
    //
    // 响应行格式：
    // <HTTP版本号> <状态码> <原因短语>
    //
    // 例如：
    // HTTP/1.1 200 OK
    cout << resp->get_http_version() << " "
         << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";
    
    // 4. 遍历并打印响应头
    // HttpHeaderCursor 用于逐个读取 HTTP 响应头字段。
    // 每次 next() 成功，都会得到一组 name/value。
    HttpHeaderCursor cursor(resp);
    string name;
    string value;
    while (cursor.next(name, value)) {
        cout << name << ": " << value << "\r\n";
    }

    // 响应头和响应体之间使用空行分隔
    cout << "\r\n";

    // 5. 解析并打印响应体
    // get_parsed_body() 会得到响应体的起始地址和长度。
    // 注意：响应体不一定是普通文本，也可能是图片、压缩数据等二进制内容。
    const void* body; // 指向响应体数据的起始位置
    size_t size;      // 响应体数据的字节数
    resp->get_parsed_body(&body, &size);

    // 这里假设响应体是文本内容，因此直接按字符串打印
    cout << static_cast<const char*>(body) << endl;
}

int main()
{   
    // 1. 创建 HTTP 客户端任务
    //
    // create_http_task() 用于创建一个异步 HTTP 请求任务。
    // 参数含义：
    // - url：请求的目标资源地址
    // - redirect_max：允许的最大重定向次数
    // - retry_max：请求失败后的最大重试次数
    // - callback：任务完成后由 workflow 框架自动调用的回调函数
    WFHttpTask* task = WFTaskFactory::create_http_task(
        // "http://www.baidu.com", // URL：代表互联网上的一个资源
        "http://stu.cskaoyan.com/",
        3, // redirect_max：最多允许重定向 3 次
        3, // retry_max：失败后最多重试 3 次
        http_callback); // 回调函数：任务完成后自动执行

    // 2. 设置 HTTP 请求参数
    HttpRequest* req = task->get_req();

    // 设置请求方法
    // GET 表示向服务器获取资源，也是默认请求方法
    req->set_method("GET");

    // 设置请求 URI
    // "/" 表示请求网站根路径，也是默认值
    req->set_request_uri("/");

    // 3. 提交任务
    // start() 之后，HTTP 请求会交给 workflow 框架异步执行，
    // 当前线程不会阻塞等待请求完成。
    task->start();
    cout << "任务已提交！" << endl;

    // 4. 阻塞主线程，防止 main 函数立即结束
    // 因为 HTTP 任务是异步执行的，如果 main 函数直接结束，
    // 进程会退出，回调函数可能还没来得及执行。
    getchar();

    // main 函数结束后进程退出
    // return 0; --> exit(0) --> _exit(0) --> 进程退出
    return 0;
}