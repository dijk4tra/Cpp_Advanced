#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;

int main()
{
    // 1. 创建序列1
    WFGoTask* task1 = WFTaskFactory::create_go_task("task", []() {
        cout << "series1: task1 done!" << endl;
    });

    WFGoTask* task2 = WFTaskFactory::create_go_task("task", []() {
        cout << "series1: task2 done!" << endl;
    });

    WFGoTask* task3 = WFTaskFactory::create_go_task("task", []() {
        cout << "series1: task3 done!" << endl;
    });

    SeriesWork* series1 = Workflow::create_series_work(task1, [](const SeriesWork* series) {
        cout << "series1: done!" << endl;
    });

    // series1->push_back(task2);
    // series1->push_back(task3);
    *series1 << task2 << task3; // 将task2和task3添加到series1中

    // 2. 创建序列2
    WFGoTask* job1 = WFTaskFactory::create_go_task("task", []() {
        cout << "series2: job1 done!" << endl;
    });

    WFGoTask* job2 = WFTaskFactory::create_go_task("task", []() {
        cout << "series2: job2 done!" << endl;
    });

    SeriesWork* series2 = Workflow::create_series_work(job1, [](const SeriesWork* series) {
        cout << "series2: done!" << endl;
    });

    series2->push_back(job2);

    // 3. 将两个序列添加到ParallelWork中
    WFFacilities::WaitGroup waitGroup(1);

    ParallelWork* parallel = Workflow::create_parallel_work([&waitGroup](const ParallelWork*) {
        cout << "ParallelWork: done!" << endl;
        waitGroup.done();
    });

    parallel->add_series(series1);
    parallel->add_series(series2);

    // 4. 启动ParallelWork
    parallel->start();

    // 5. 主线程阻塞, 等待ParallelWork完成
    waitGroup.wait();

}
