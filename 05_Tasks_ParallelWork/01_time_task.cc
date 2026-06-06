#include <iostream>
#include <unistd.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;

// Q: 如何取消周期性的定时器任务
void timer_callback(WFTimerTask* task)
{
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cout << "Task cancelled, state: " << state << endl;
        return;
    }

    cout << "Timer Triggered!" << endl;

    WFTimerTask* next_task = WFTaskFactory::create_timer_task("timer", 1, 0, timer_callback);
    series_of(task)->push_back(next_task);
}

int main()
{
    // 1. 创建有名定时器任务
    WFTimerTask* task = WFTaskFactory::create_timer_task("timer", 3, 0, timer_callback);

    // 2. 启动定时器任务
    WFFacilities::WaitGroup waitGroup(1);

    SeriesWork* series = Workflow::create_series_work(task, [&waitGroup](const SeriesWork*){
        waitGroup.done();
    });

    series->start();

    // 3. 主线程等待任务完成
    sleep(10);
    // series->cancel(); // 取消整个任务系列, 使用这种方式取消周期性定时器任务不太好

    WFTaskFactory::cancel_by_name("timer");

    waitGroup.wait();

}
