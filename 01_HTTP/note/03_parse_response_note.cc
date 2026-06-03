// 角色：HTTP 客户端
// 任务：使用 workflow 发送 HTTP 请求，并解析 HTTP 响应报文
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
// Date: Tue, 21 May 2024 10:00:00 GMT
// Content-Type: text/html
// Content-Length: 123
// Server: nginx
//
// <html>
//     <body>Hello World</body>
// </html>
//
// 说明：
//
// HTTP 响应报文由三部分组成：
//
// 1. 响应行 Status Line
//    包含 HTTP 版本号、状态码、原因短语。
//
// 2. 响应头 Headers
//    描述响应体的类型、长度、服务器信息、缓存策略等。
//
// 3. 响应体 Body
//    服务器真正返回的数据，比如 HTML、JSON、图片、文件等。

#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTaskFactory.h>

#include <iomanip>
#include <iostream>
#include <string>

using namespace std;
using namespace protocol;


// ============================================================
// 全局 WaitGroup
// ============================================================
//
// workflow 的 HTTP 任务是异步执行的。
//
// 也就是说：
//
// task->start();
//
// 只负责把任务提交给 workflow 框架，
// 不会一直阻塞等待 HTTP 响应回来。
//
// 如果 main 函数不等待，程序可能会在响应还没回来之前就结束。
//
// 所以这里使用 WFFacilities::WaitGroup 来等待异步任务完成。
//
// WaitGroup wait_group(1) 表示当前有 1 个异步任务需要等待。
//
// 当 HTTP 任务完成后，在回调函数中调用：
//
// wait_group.done();
//
// 主线程中的：
//
// wait_group.wait();
//
// 才会解除阻塞，程序继续向下执行。
static WFFacilities::WaitGroup wait_group(1);


// ============================================================
// 打印二进制响应体
// ============================================================
//
// 为什么需要这个函数？
//
// 因为 HTTP 响应体不一定是普通文本。
//
// 它可能是：
//
// 1. HTML
// 2. JSON
// 3. 普通字符串
// 4. 图片
// 5. 压缩包
// 6. 音频、视频等二进制数据
//
// 如果响应体是二进制数据，里面可能包含不可见字符，
// 也可能包含 '\0'。
//
// 直接 cout 打印二进制内容，可能会乱码，
// 也可能看起来像什么都没输出。
//
// 所以这里把每个字节转换成十六进制打印出来，
// 方便观察响应体的原始字节内容。
void print_body_as_hex(const void *body, size_t size)
{
    // body 是 const void* 类型，表示一段原始内存。
    //
    // 为了按字节访问，需要转换成 const unsigned char*。
    //
    // unsigned char 表示无符号字节，
    // 避免 char 被当成负数处理。
    const unsigned char *p = static_cast<const unsigned char *>(body);

    cout << "Body hex: ";

    // size 表示响应体的真实字节数。
    //
    // 注意：
    // 不要用 strlen(...) 计算响应体大小。
    //
    // 因为二进制数据中可能包含 '\0'，
    // strlen 遇到 '\0' 会提前停止。
    for (size_t i = 0; i < size; ++i)
    {
        // hex：以十六进制输出
        // setw(2)：每个字节至少占 2 位
        // setfill('0')：不足 2 位时前面补 0
        // static_cast<int>(p[i])：把字节转换成整数再打印
        cout << hex
             << setw(2)
             << setfill('0')
             << static_cast<int>(p[i])
             << " ";
    }

    // 打印完十六进制之后，恢复成十进制输出。
    //
    // 否则后面打印数字时，也会继续使用十六进制。
    cout << dec << endl;
}


// ============================================================
// HTTP 任务完成后的回调函数
// ============================================================
//
// workflow 中的 HTTP 客户端任务是异步任务。
//
// 当 HTTP 请求执行完成后，不管成功还是失败，
// workflow 都会自动调用这个回调函数。
//
// 参数：
//
// WFHttpTask *task
//
// 表示本次 HTTP 客户端任务。
// 通过 task 可以拿到：
//
// 1. 任务状态
// 2. 错误信息
// 3. HTTP 请求对象
// 4. HTTP 响应对象
void http_callback(WFHttpTask *task)
{
    // ============================================================
    // 1. 判断 HTTP 任务是否成功
    // ============================================================
    //
    // get_state() 用来获取任务状态。
    //
    // 常见情况：
    //
    // WFT_STATE_SUCCESS
    // 表示任务成功。
    //
    // 其他状态可能表示：
    //
    // 1. DNS 解析失败
    // 2. TCP 连接失败
    // 3. 连接超时
    // 4. SSL 错误
    // 5. HTTP 协议错误
    // 6. 用户主动取消任务等
    int state = task->get_state();

    if (state != WFT_STATE_SUCCESS)
    {
        // 如果任务失败，可以通过：
        //
        // task->get_error()
        //
        // 获取具体错误码。
        //
        // WFGlobal::get_error_string(...)
        // 可以把错误状态和错误码转换成可读的错误信息。
        cout << "HTTP task failed: "
             << WFGlobal::get_error_string(state, task->get_error())
             << endl;

        // 注意：
        //
        // 即使任务失败，也必须调用 wait_group.done()。
        //
        // 因为 main 函数中正在 wait_group.wait() 等待任务结束。
        //
        // 如果这里直接 return，而不调用 done()，
        // 主线程会一直阻塞，程序无法结束。
        wait_group.done();
        return;
    }


    // ============================================================
    // 2. 获取 HTTP 响应对象
    // ============================================================
    //
    // 任务成功，说明客户端已经收到了服务器返回的 HTTP 响应。
    //
    // task->get_resp()
    // 可以拿到响应对象 HttpResponse。
    //
    // 后面解析响应行、响应头、响应体，
    // 都是通过这个 resp 对象完成的。
    HttpResponse *resp = task->get_resp();

    cout << "========== HTTP Response ==========" << endl;


    // ============================================================
    // a. 解析响应行 Status Line
    // ============================================================
    //
    // HTTP 响应报文的第一行叫做“响应行”。
    //
    // 格式：
    //
    // <HTTP版本号> <状态码> <原因短语>
    //
    // 例如：
    //
    // HTTP/1.1 200 OK
    //
    // 其中：
    //
    // HTTP版本号    HTTP/1.1
    // 状态码        200
    // 原因短语      OK
    //
    // 常见状态码：
    //
    // 200 OK
    // 表示请求成功。
    //
    // 301 Moved Permanently
    // 表示永久重定向。
    //
    // 302 Found
    // 表示临时重定向。
    //
    // 404 Not Found
    // 表示资源不存在。
    //
    // 500 Internal Server Error
    // 表示服务器内部错误。

    cout << "========== Status Line ==========" << endl;

    // 按照 HTTP 响应行格式打印
    cout << resp->get_http_version() << " "
         << resp->get_status_code() << " "
         << resp->get_reason_phrase() << "\r\n";

    cout << endl;

    // 分别打印响应行中的三个部分
    cout << "http version : " << resp->get_http_version() << endl;
    cout << "status code  : " << resp->get_status_code() << endl;
    cout << "reason phrase: " << resp->get_reason_phrase() << endl;

    cout << endl;


    // ============================================================
    // b. 解析响应头 Headers
    // ============================================================
    //
    // 响应头位于响应行之后。
    //
    // 格式：
    //
    // Header-Name: Header-Value
    //
    // 例如：
    //
    // Date: Tue, 21 May 2024 10:00:00 GMT
    // Content-Type: text/html
    // Content-Length: 123
    // Server: nginx
    // Connection: close
    //
    // 响应头的作用：
    //
    // 1. Content-Type
    //    告诉客户端响应体是什么类型。
    //    例如 text/html、application/json、image/png。
    //
    // 2. Content-Length
    //    告诉客户端响应体有多少字节。
    //
    // 3. Server
    //    告诉客户端服务器软件信息。
    //
    // 4. Connection
    //    表示连接是否保持。
    //
    // HttpHeaderCursor 用来遍历所有 HTTP 响应头。

    cout << "========== Headers ==========" << endl;

    HttpHeaderCursor cursor(resp);

    string name;
    string value;

    // cursor.next(name, value)
    //
    // 如果还有下一个响应头，就返回 true，
    // 并把响应头名字放到 name 中，
    // 把响应头的值放到 value 中。
    //
    // 如果没有更多响应头，就返回 false，循环结束。
    while (cursor.next(name, value))
    {
        cout << name << ": " << value << "\r\n";
    }

    // 响应头和响应体之间有一个空行
    cout << endl;


    // ============================================================
    // c. 解析响应体 Body
    // ============================================================
    //
    // 响应体位于响应头后面的空行之后。
    //
    // 常见响应体类型：
    //
    // 1. HTML 页面
    // 2. JSON 数据
    // 3. XML 数据
    // 4. 普通文本
    // 5. 图片、音频、视频
    // 6. 文件、压缩包等二进制数据
    //
    // get_parsed_body(&body, &size)
    //
    // 可以拿到响应体内容。
    //
    // 参数说明：
    //
    // const void *body
    // 指向响应体数据的起始位置。
    //
    // size_t size
    // 表示响应体真实大小，单位是字节。

    cout << "========== Body ==========" << endl;

    const void *body = nullptr;
    size_t size = 0;

    resp->get_parsed_body(&body, &size);

    cout << "Body size: " << size << " bytes" << endl;

    if (body == nullptr || size == 0)
    {
        // 有些响应可能没有响应体。
        //
        // 例如：
        //
        // 1. 某些 204 No Content 响应
        // 2. 某些 HEAD 请求的响应
        // 3. 某些错误响应
        cout << "empty body" << endl;
    }
    else
    {
        // ========================================================
        // d. 按文本方式查看响应体
        // ========================================================
        //
        // 如果响应体是 HTML、JSON、XML、普通文本，
        // 可以按文本方式打印。
        //
        // 注意：
        //
        // 不要直接写：
        //
        // cout << static_cast<const char *>(body) << endl;
        //
        // 原因：
        //
        // HTTP 响应体不一定以 '\0' 结尾。
        //
        // cout 打印 char* 时，会一直打印到遇到 '\0' 为止。
        //
        // 如果 body 中间没有 '\0'，
        // 就可能越界读取。
        //
        // 正确做法：
        //
        // 使用 body 指针和 size 长度构造 std::string。
        //
        // 这样可以保证只读取 size 个字节。
        string body_text(static_cast<const char *>(body), size);

        cout << "body as text:" << endl;
        cout << body_text << endl;

        cout << endl;


        // ========================================================
        // e. 按二进制方式查看响应体
        // ========================================================
        //
        // 如果响应体是图片、压缩包、音频、视频等二进制内容，
        // 直接按文本打印可能会乱码。
        //
        // 对于二进制响应体，更推荐：
        //
        // 1. 查看 body size
        // 2. 按字节打印十六进制
        //
        // 这样可以观察真实的原始字节。
        cout << "body as binary:" << endl;
        print_body_as_hex(body, size);
    }

    cout << "===================================" << endl;


    // ============================================================
    // 3. 通知主线程：HTTP 任务已经完成
    // ============================================================
    //
    // main 函数中调用了：
    //
    // wait_group.wait();
    //
    // 它会一直等待任务完成。
    //
    // 这里调用：
    //
    // wait_group.done();
    //
    // 表示这个 HTTP 异步任务已经结束。
    //
    // 然后主线程就可以继续执行并退出程序。
    wait_group.done();
}


int main()
{
    // ============================================================
    // 1. 创建 HTTP 客户端任务
    // ============================================================
    //
    // WFTaskFactory::create_http_task(...)
    //
    // 用来创建一个 HTTP 客户端任务。
    //
    // 参数说明：
    //
    // 第 1 个参数：
    // URL，表示客户端要访问的网络资源。
    //
    // 例如：
    //
    // http://www.baidu.com
    // http://www.example.com/index.html
    //
    // 第 2 个参数：
    // redirect_max，最大重定向次数。
    //
    // 如果服务器返回 301、302 等重定向响应，
    // workflow 最多可以自动跟随多少次重定向。
    //
    // 第 3 个参数：
    // retry_max，最大重试次数。
    //
    // 如果请求失败，workflow 最多可以重试多少次。
    //
    // 第 4 个参数：
    // 回调函数。
    //
    // 当 HTTP 任务完成后，
    // workflow 会自动调用这个函数。
    WFHttpTask *task = WFTaskFactory::create_http_task(
        "http://www.baidu.com", // URL，表示要访问的资源
        3,                      // redirect_max，最大重定向次数
        3,                      // retry_max，请求失败后的最大重试次数
        http_callback           // 回调函数，任务完成后自动调用
    );


    // ============================================================
    // 2. 设置 HTTP 请求
    // ============================================================
    //
    // 客户端要发送给服务器的是 HTTP 请求。
    //
    // task->get_req()
    // 可以拿到本次任务中的 HTTP 请求对象。
    //
    // 后面可以通过 req 设置：
    //
    // 1. 请求方法
    // 2. 请求 URI
    // 3. 请求头
    // 4. 请求体
    HttpRequest *req = task->get_req();


    // ============================================================
    // a. 设置请求方法 Method
    // ============================================================
    //
    // HTTP 请求方法表示客户端想对资源执行什么操作。
    //
    // 常见方法：
    //
    // GET
    // 获取资源。
    //
    // POST
    // 提交数据。
    //
    // PUT
    // 更新资源。
    //
    // DELETE
    // 删除资源。
    //
    // 本练习是 HTTP 客户端获取网页内容，
    // 所以使用 GET 方法。
    req->set_method("GET");


    // ============================================================
    // b. 设置请求 URI
    // ============================================================
    //
    // HTTP 请求行格式：
    //
    // <请求方法> <URI> <HTTP版本号>
    //
    // 例如：
    //
    // GET / HTTP/1.1
    //
    // 这里的 "/" 表示访问网站根路径。
    //
    // 如果要访问：
    //
    // http://www.example.com/index.html
    //
    // 那么 URI 通常是：
    //
    // /index.html
    req->set_request_uri("/");


    // ============================================================
    // c. 设置请求头 Headers
    // ============================================================
    //
    // 请求头用来告诉服务器客户端的一些信息。
    //
    // 格式：
    //
    // Header-Name: Header-Value
    //
    // 例如：
    //
    // User-Agent: workflow-http-client
    // Accept: */*
    // Connection: close
    //
    // User-Agent
    // 表示客户端身份。
    //
    // Accept
    // 表示客户端可以接收什么类型的数据。
    //
    // Connection: close
    // 表示本次响应结束后关闭连接。
    req->add_header_pair("User-Agent", "workflow-http-client");
    req->add_header_pair("Accept", "*/*");
    req->add_header_pair("Connection", "close");


    // ============================================================
    // 3. 启动 HTTP 异步任务
    // ============================================================
    //
    // task->start()
    //
    // 表示把任务提交给 workflow 框架执行。
    //
    // 注意：
    //
    // start() 不表示响应已经回来。
    //
    // 它只是启动异步任务。
    //
    // 真正收到响应后，
    // workflow 会自动调用前面注册的回调函数 http_callback。
    task->start();

    cout << "任务已提交，等待 HTTP 响应..." << endl;


    // ============================================================
    // 4. 等待 HTTP 任务完成
    // ============================================================
    //
    // 因为 workflow 的 HTTP 请求是异步执行的，
    // 所以 main 线程需要等待任务完成。
    //
    // wait_group.wait()
    //
    // 会阻塞当前线程。
    //
    // 直到回调函数中调用：
    //
    // wait_group.done();
    //
    // main 线程才会继续往下执行。
    wait_group.wait();


    // ============================================================
    // 5. 程序结束
    // ============================================================
    //
    // HTTP 响应已经解析完成。
    //
    // main 函数返回 0，表示程序正常结束。
    cout << "主线程结束。" << endl;

    return 0;
}