/*
 * 实现一个静态资源服务器
 * 所谓静态资源，就是不会变换的资源，一般是以文件的形式存放在服务器的磁盘上
 * 这个程序主要展示了磁盘IO任务的用法
 *
 * Linux操作系统支持一套效率很高，CPU占用非常少的异步IO系统调用。
 * 在Linux系统下使用我们的框架将默认使用这套接口。
 */
#include "common.h"
#include <cassert>
#include <fcntl.h>
#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace std::placeholders;
using namespace protocol;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

void pread_callback(WFFileIOTask* task, HttpResponse* resp, string filename) // 使用 bind (还可以使用user_data和SeriesContext)
{
    /*
    struct FileIOArgs
    {
        int fd;
        void *buf;
        size_t count;
        off_t offset;
    };
    */
    // 1. 获取参数和返回值
    FileIOArgs* args = task->get_args();
    close(args->fd); // 不要忘记关闭文件描述符!
    long bytes_read = task->get_retval(); // 获取实际读取的字节数

    // 2. 判断任务的状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        resp->set_status_code("500");
        resp->append_output_body("<html>500 Server Internal Error.</html>");
        return;
    }

    // 3. 将文件内容(buf)追加到响应体中
    // 慎重使用'_nocopy': 需要保证args->buf的生命周期长于httpTask
    resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
    resp->append_output_body_nocopy(args->buf, bytes_read);

}


void process(WFHttpTask* httpTask)
{
    // GET /dir/a.txt HTTP/1.1
    // 1. 解析请求
    HttpRequest* req = httpTask->get_req();
    string uri = req->get_request_uri();
    auto pos = uri.find('?');
    string path = uri.substr(0, pos);
    cout << "[path] " << path << endl;

    // 路径映射
    if (path == "/") {
        path += "index.html";
    }
    path = "resources" + path;
    cout << "[path] " << path << endl;

    // 获取文件名
    pos = path.find_last_of('/');
    string filename = path.substr(pos + 1);
    cout << "[filename] " << filename << endl;

    // 2. 创建 pread 任务
    HttpResponse* resp = httpTask->get_resp();
    resp->add_header_pair("Server", "Sogou C++ Workflow Server");

    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        resp->set_status_code("404");
        resp->append_output_body("<html>404 Not Found.</html>");
        return;
    }

    // 获取文件的大小
    struct stat statbuf;
    fstat(fd, &statbuf);
    size_t size = statbuf.st_size;
    cout << "size: " << size << endl;

    char* buf = (char*)malloc(size); // buf: 指针变量, 局部变量
    assert(buf != NULL && "malloc failed");

    // 指针变量一律值传递!!!(符合99.9%的情况)
    httpTask->set_callback([buf](WFHttpTask* task){ // 这里只能值捕获, 因为buf是局部变量(指针变量一律值传递)
        free(buf);
    });

    WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(
        fd,
        buf,
        size,
        0,
        std::bind(pread_callback, std::placeholders::_1, resp, filename)); // resp是指针变量, 必须值传递

    // 3. 将 preadTask 加入到序列中
    // preadTask->start(); // 直接启动preadTask? ERROR: 这样会另起一个新序列, preadTask会和httpTask并发执行
    series_of(httpTask)->push_back(preadTask);
}


int main(int argc, char* argv[])
{
    // 1. 注册信号处理函数
    signal(SIGINT, sig_handler);

    // 2. 使用默认参数, 创建 HTTP 服务器
    WFHttpServer server(process);

    // 3. 启动 HTTP 服务器，绑定通配符地址，并监听在8888端口
    if (server.start(8888) == 0){
        // 让主线程阻塞
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "ERROR: Server start FAILED!" << endl;
        exit(1);
    }

    return 0;
}
