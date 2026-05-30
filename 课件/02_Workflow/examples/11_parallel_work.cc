#include <iostream>
#include <workflow/WFFacilities.h> // for WFFacilities::WaitGroup
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h> // for ParallelWork

using namespace std;

int main()
{
    // 1. 创建SeriesWork* series1，并加入三个task
    // series1: task1 -> task2 ->task3;
    WFGoTask* task1 = WFTaskFactory::create_go_task("series1", []() {
        cout << "series1: task 1 done." << endl;
    });
    WFGoTask* task2 = WFTaskFactory::create_go_task("series1", []() {
        cout << "series1: task 2 done." << endl;
    });
    WFGoTask* task3 = WFTaskFactory::create_go_task("series1", []() {
        cout << "series1: task 3 done." << endl;
    });
    SeriesWork* series1 = Workflow::create_series_work(task1, [](const SeriesWork* series) {
        cout << "series1: done!" << endl;
    });
    *series1 << task2 << task3;
    // 2. 创建 SeriesWork* series2，并加入两个job.
    // series2: job1 -> job2;
    WFGoTask* job1 = WFTaskFactory::create_go_task("series2", []() {
        cout << "series2: job 1 done." << endl;
    });
    WFGoTask* job2 = WFTaskFactory::create_go_task("series2", []() {
        cout << "series2: job 2 done." << endl;
    });
    SeriesWork* series2 = Workflow::create_series_work(job1, [](const SeriesWork* series) {
        cout << "series2: done!" << endl;
    });
    series2->push_back(job2);
    // 3. 创建ParallelWork，并将series1和series2加入其中
    WFFacilities::WaitGroup waitGroup(1);
    ParallelWork* parallel = Workflow::create_parallel_work([&waitGroup](const ParallelWork* work) {
        cout << "ParallelWork done!" << endl;
        waitGroup.done();
    });
    parallel->add_series(series1);
    parallel->add_series(series2);
    // 4. 启动ParallelWork，交给框架并行执行里面的序列
    parallel->start();
    // 5. 主线程等待ParallelWork完成
    waitGroup.wait();
    cout << "主线程退出" << endl;
    return 0;
}
