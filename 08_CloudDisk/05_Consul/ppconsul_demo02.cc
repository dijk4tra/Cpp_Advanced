// Consul: 实现服务的自动注册
#include <ppconsul/agent.h>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using ppconsul::Consul;
using namespace ppconsul::agent;
using namespace std::placeholders;
using namespace wfrest;
using namespace std;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

void timer_callback(WFTimerTask* task, Agent& agent)
{
    if (task->get_state() != WFT_STATE_SUCCESS) {
        return;
    }
    // 健康检查
    agent.servicePass("my-service-1");
    WFTimerTask* next = WFTaskFactory::create_timer_task(
        "health-check",
        9,
        0,
        std::bind(timer_callback, _1, std::ref(agent)));

    series_of(task)->push_back(next);
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer server;

    server.GET("/*", [](const HttpReq* req, HttpResp* resp) {
        resp->String("Hello world!");
    });

    if (server.start(8888) == 0) {
        // 启动成功：注册服务的信息
        Consul consul("http://127.0.0.1:8500", ppconsul::kw::dc = "dc1");
        Agent agent(consul);
        agent.registerService(
            kw::id = "my-service-1", // 实例的id
            kw::name = "my-service", // 服务的名字
            kw::address = "127.0.0.1",
            kw::port = 8888, // 端口
            kw::check = TtlCheck { std::chrono::seconds(10) }); // TTL: time to live

        // 健康检查
        agent.servicePass("my-service-1");
        WFTimerTask* task = WFTaskFactory::create_timer_task(
            "health-check",
            9,
            0,
            std::bind(timer_callback, _1, std::ref(agent)));
        task->start();

        waitGroup.wait();
        // 取消定时器任务
        WFTaskFactory::cancel_by_name("health-check");
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
}
