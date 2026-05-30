/**
 *  WFGoTask 是模仿 go 语言协程实现的，它代表一种计算任务
 *
 *  ```
 *  class WFTaskFactory
 *  {
 *      ...
 *  public:
 *      template<class FUNC, class... ARGS>
 *      static WFGoTask* create_go_task(const std::string& queue_name,
 *                                      FUNC&& func, ARGS&&... args);
 *      ...
 *  }
 *  ```
 *  queue_name: 计算队列名，不要纠结
 *  func: 要执行的计算任务，可以是函数指针、函数对象、lambda函数、类的成员函数等任意可调用的对象
 *  args: func的参数。当 func 是类的非静态成员函数时，args的第一个成员必须是对象的地址
 */
#include "common.h"
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>

using namespace std;

void add(int a, int b, int& result)
{
    // 模拟耗时的计算
    sleep(3);
    result = a + b;
}

int main()
{
    // 1. 创建WFGoTask
    int a = 1, b = 2;
    int result;
    // ERROR: 参数是值传递的
    // WFGoTask* task = WFTaskFactory::create_go_task("queue1", add, a, b, result);
    WFGoTask* task = WFTaskFactory::create_go_task("queue1", add, a, b, ref(result));
    // 2. 设置WFGoTask任务的回调函数
    WFFacilities::WaitGroup waitGroup(1);
    task->set_callback([&](WFGoTask* task) {
        cout << a << " + " << b << " = " << result << endl;
        cout << "WFGoTask执行完毕" << endl;
        waitGroup.done();
    });
    // 3. 启动任务：交给框架异步执行
    task->start();
    // 4. 主线程等待任务执行完毕
    waitGroup.wait();
    return 0;
}
