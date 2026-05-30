#include "common.h"
#include <iostream>
#include <ppconsul/agent.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;
using ppconsul::Consul;
using namespace ppconsul::agent;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

void timer_callback(WFTimerTask* task)
{
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        return;
    }
    SeriesWork* series = series_of(task);
    Agent* agent = static_cast<Agent*>(series->get_context());

    // 发送心跳包
    agent->servicePass("UserService1");
    // 创建另一个定时任务
    WFTimerTask* nextTask = WFTaskFactory::create_timer_task("health_check", 9, 0, timer_callback);
    series->push_back(nextTask);
}

int main()
{
    HttpServer server;

    if (server.start(8888) == 0) {

        // 指定注册中心 Consul 的ip地址，端口和数据中心
        Consul consul("http://127.0.0.1:8500", ppconsul::kw::dc = "dc1");
        // 创建Consul客户端代理
        Agent agent(consul);
        // 注册实例的元信息
        agent.registerService(
            kw::id = "UserService1",
            kw::name = "UserService",
            kw::address = "127.0.0.1",
            kw::port = 8888,
            kw::check = TtlCheck { chrono::seconds(10) });

        // 定时发送心跳包
        WFTimerTask* timerTask = WFTaskFactory::create_timer_task("health_check", 9, 0, timer_callback);

        SeriesWork* series = Workflow::create_series_work(timerTask, nullptr);
        series->set_context(&agent); // 设置序列的上下文
        series->start();

        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
