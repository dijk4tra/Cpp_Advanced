#include "common.h"
#include "example.srpc.h"
#include <workflow/WFFacilities.h>

using namespace std;
using namespace srpc;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
}

// 回调函数：收到RPC响应后才会执行
static void echo_done(EchoResponse* response, srpc::RPCContext* context)
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
    // 2. 发送rpc请求
    EchoRequest req;
    req.set_message("Hello, srpc!");
    req.set_name("peanut");
    // 像本地方法一样调用远程方法
    client.Echo(&req, echo_done);

    cout << "after client.Echo()" << endl;
    // 3. 主线程等待
    waitGroup.wait();
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
