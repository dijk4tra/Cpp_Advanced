#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;

// 引用:
// int& --> 左值引用: 只能绑定到左值(变量)上
// const int& --> const左值引用: 既能绑定左值, 也能绑定右值, 但不能修改值
// int&& --> 右值引用: 只能绑定到右值(临时对象)上
// T&& --> 通用引用: 既能绑定到左值，也能绑定到右值


void add(int a, int b, int& c)
{
    sleep(3); // 模拟耗时计算
    c = a + b;
    cout << "add: c = " << c << endl;
}


int main()
{
    // 1. 创建WFGoTask任务
    int a = 3, b = 3, c = 0;
    WFGoTask* task = WFTaskFactory::create_go_task("q1", add, a, b, std::ref(c));
    // Q: WFGoTask没有回调函数吗?
    task->set_callback([&c](WFGoTask*) {
        cout << "callback: c = " << c << endl;
    });

    // 2. 启动任务
    WFFacilities::WaitGroup waitGroup(1);

    SeriesWork* series = Workflow::create_series_work(task, [&waitGroup](const SeriesWork*){
        waitGroup.done();
    });

    series->start();

    // 3. 主线程等待任务完成
    waitGroup.wait();
    cout << "main: c = " << c << endl;

}
