/*
 * 这一节，我将带大家实现一个静态资源服务器。
 * 所谓静态资源，就是不会变换的资源，一般是以文件的形式存放在服务器的磁盘上。
 * 这个程序主要展示了磁盘IO任务的用法。
 *
 * Linux操作系统支持一套效率很高，CPU占用非常少的异步IO系统调用。在Linux系统下使用我们的框架将默认使用这套接口。
 */
#include "common.h"
#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFHttpServer.h>

using namespace std;
using namespace std::placeholders;
using namespace protocol;

void pread_callback(WFFileIOTask* task, HttpResponse* resp, string filename)
{
    /*
     * struct FileIOArgs
     * {
     *     int fd;
     *     void* buf;
     *     size_t count;
     *     off_t offset;
     * };
     */
    FileIOArgs* args = task->get_args();
    close(args->fd); // 不要忘记关闭文件描述符！
    // 返回已读取的字节数目，任务已完成，该方法不会阻塞
    long bytes = task->get_retval();
    if (task->get_state() != WFT_STATE_SUCCESS) {
        resp->set_status_code("500");
        resp->append_output_body("<html>500 Internal Server Error.</html>");
    } else {
        /* 慎重使用 '_nocopy' */
        // 需要保证args->buf的声明周期长于resp所在的WFHttpTask!
        resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
        resp->append_output_body_nocopy(args->buf, bytes);
    }
}

void process(WFHttpTask* task)
{
    // 1. 解析请求，获取用户要下载的文件路径和文件名
    HttpRequest* req = task->get_req();
    string uri = req->get_request_uri();
    // 截取路径部分
    size_t pos = uri.find('?');
    string path;
    if (pos != string::npos) {
        path = uri.substr(0, pos);
    } else {
        path = uri;
    }
    // 如果路径为"/"，获取首页index.html
    if (path == "/") {
        path += "index.html";
    }
    // 将URI中的路径 --> 服务器文件系统的路径
    path = "resources" + path;
    // 获取文件名
    string filename = path.substr(path.find_last_of('/') + 1);
    // 打印调试信息
    cout << "path: " << path << ", filename: " << filename << endl;
    // 2. 设置通用的响应头部（成功和失败都需要设置的头部）
    HttpResponse* resp = task->get_resp();
    resp->add_header_pair("Server", "Sogou C++ Workflow Server");
    // 3. 创建WFFileIOTask，读取文件内容
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        resp->set_status_code("404");
        resp->append_output_body("<html>404 Not Found.</html>");
        return;
    }
    // 创建存放数据的buf
    size_t size = lseek(fd, 0, SEEK_END);
    void* buf = malloc(size);
    assert(buf != nullptr && "malloc failed"); // 断言：buf不为空，否则程序崩溃
    // 这里我们利用任务的 user_date 来传递数据
    // 如果是更复杂的情况，请使用序列的上下文来传递数据
    task->user_data = buf;
    // free in task's callback
    task->set_callback([](WFHttpTask* task) {
        free(task->user_data);
    });
    WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(
        fd, // 文件描述符
        buf, // 存放数据的buf
        size, // 最多读取的字节数目
        0, // offset: 偏移量
        bind(pread_callback, _1, resp, filename) // 回调函数
    );
    // 将WFFileIOTask追加到序列中
    series_of(task)->push_back(preadTask);
}

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    // 1.注册信号处理函数
    signal(SIGINT, sig_handler);
    // 2.使用默认参数，创建HTTP服务器
    WFHttpServer server(process);
    // 3. 启动HTTP服务器，绑定通配符地址，并监听在8080端口
    if (server.start(8080) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
