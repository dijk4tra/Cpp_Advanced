/**
 * wfrest支持两种类型的Handler：
 * using Handler = std::function<void(const HttpReq*, HttpResp*)>;
 * using SeriesHandler = std::function<void<const HttpReq*, HttpResp*, SeriesWork*)>;
 * 使用SeriesHandler可以轻易地与workflow集成
 */

#include <iostream>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler); // 注册Ctrl+C信号处理

    HttpServer server;

    server.GET("/series", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        // 使用Workflow创建定时器任务
        WFTimerTask* timerTask = WFTaskFactory::create_timer_task(3, 0, [](WFTimerTask*) {
            cout << "定时器任务完成(3秒)" << endl;
        });
        // 往SeriesWork中添加timerTask，也可以添加各种各样的其它任务
        series->push_back(timerTask);
        // timerTask->start();
        resp->String("Hello, SeriesTask!\n");
    });

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
    }
    return 0;
}
