#include "common.h"
#include "example.srpc.h"
#include <srpc/rpc_define.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace srpc;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
}

// 回调函数：收到RPC响应后才会执行
static void echo_callback(EchoResponse* response, srpc::RPCContext* context)
{
    // 1. 判断RPC请求是否成功
    if (!context->success()) {
        cerr << "error code: " << context->get_error()
             << ", error msg: " << context->get_errmsg() << endl;
    }
    // 2. 打印响应
    cout << "EchoResponse{ message=" << response->message() << " }" << endl;
}

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    // 注册信号处理函数
    signal(SIGINT, sig_handler);
    // 1. 创建srpc客户端
    const char* ip = "127.0.0.1"; // srpc服务器的IP地址
    unsigned short port = 1314; // srpc服务器监听的端口
    Example::SRPCClient client(ip, port);
    // 2. 创建SRPCClient任务
    SRPCClientTask* task = client.create_Echo_task(echo_callback);
    // 设置请求
    EchoRequest req;
    req.set_message("Hello, srpc!");
    req.set_name("peanut");
    task->serialize_input(&req);

    // 3. 启动任务（也可以和其它任务一起编排，组成串行流或并行流）
    task->start();

    cout << "after client.Echo()" << endl;
    // 4. 主线程等待
    waitGroup.wait();
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
