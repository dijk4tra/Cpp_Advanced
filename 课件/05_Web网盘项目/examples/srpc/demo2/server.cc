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

class ExampleService : public Example::Service {
public:
    // 实现远程过程调用Echo
    void Echo(EchoRequest* request, EchoResponse* response, srpc::RPCContext* ctx) override
    {
        // 1. 解析请求，这里仅仅是打印请求信息
        cout << "EchoRequest{ message=" << request->message()
             << ", name=" << request->name() << " }" << endl;
        // 2. 处理业务逻辑
        sleep(3); // 模拟耗时的业务逻辑
        // 3. 生成响应
        response->set_message("Hi, " + request->name());
    }
};

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // 1. 注册信号处理函数
    signal(SIGINT, sig_handler);
    // 2. 使用默认参数，创建srpc服务器
    SRPCServer server;
    // 3. 注册服务
    // 一个SRPCServer可以注册多个Service，一个Service可以包含多个RPC
    ExampleService service;
    server.add_service(&service);
    // 此处可添加其它的service...

    // 4. 启动SRPCServer
    if (server.start(1314) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
    }

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
