#include "common.h"
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>

using namespace std;

void timer_callback(WFTimerTask* task)
{
    // 尽管任务被取消，任务的回调函数依然会执行！
    if (task->get_state() != WFT_STATE_SUCCESS) {
        cout << "定时任务被取消" << endl;
        return;
    }
    cout << "Timer TRIGGERED!" << endl;
    WFTimerTask* next = WFTaskFactory::create_timer_task("demo", 1, 0, timer_callback);
    series_of(task)->push_back(next);
}

int main()
{
    // Workflow将时间的流逝抽象成了定时任务：WFTimerTask
    // 1. 创建定时任务
    WFTimerTask* task = WFTaskFactory::create_timer_task("demo", 3, 0, timer_callback);
    // 2. 将定时任务放入序列中，并启动序列
    WFFacilities::WaitGroup waitGroup(1);
    SeriesWork* series = Workflow::create_series_work(task, [&waitGroup](const SeriesWork*) {
        cout << "SeriesWork终止" << endl;
        waitGroup.done();
    });
    series->start();
    // 3. 主线程休眠10s
    sleep(10);
    // 4. 取消任务
    WFTaskFactory::cancel_by_name("demo");
    // 5. 主线程等待序列终止
    waitGroup.wait();
    cout << "主线程退出" << endl;
    return 0;
}
