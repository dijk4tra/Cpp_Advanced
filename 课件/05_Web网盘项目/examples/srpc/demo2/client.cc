#include "common.h"
#include "example.pb.h"
#include "example.srpc.h"
#include <srpc/rpc_context.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace srpc;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signo)
{
    waitGroup.done();
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

    EchoResponse resp;
    RPCSyncContext ctx;
    client.Echo(&req, &resp, &ctx); // 同步调用

    if (!ctx.success) {
        cerr << "error code: " << ctx.error
             << ", error msg: " << ctx.errmsg << endl;
    } else {
        cout << "EchoResponse{ message=" << resp.message() << " }" << endl;
    }

    cout << "after client.Echo()" << endl;
    // 3. 主线程等待
    waitGroup.wait();
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
