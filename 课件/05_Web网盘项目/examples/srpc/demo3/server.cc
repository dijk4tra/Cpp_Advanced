#include "common.h"
#include "example.srpc.h"
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace protocol;
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
        // 2. 处理业务逻辑（获取sogou首页）
        WFHttpTask* httpTask = WFTaskFactory::create_http_task("http://www.sogou.com", 3, 3,
            // 3. 生成响应
            [response](WFHttpTask* task) {
                int state = task->get_state();
                if (state != WFT_STATE_SUCCESS) {
                    response->set_message(string("Error: ") + WFGlobal::get_error_string(state, task->get_error()));
                    return;
                }
                // HTTP任务成功
                const void* body;
                size_t size;
                HttpResponse* resp = task->get_resp();
                resp->get_parsed_body(&body, &size);
                response->set_message((const char*)body);
            });

        // httpTask->start(); // 错误!
        // 通过上下文获取处理RPC请求的任务序列
        SeriesWork* series = ctx->get_series();
        series->push_back(httpTask);
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
