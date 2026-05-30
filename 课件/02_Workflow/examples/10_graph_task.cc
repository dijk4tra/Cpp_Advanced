#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>

using namespace std;

void http_callback(WFHttpTask* task)
{
    size_t* size = static_cast<size_t*>(task->user_data);
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        *size = (size_t)-1;
        return;
    }
    const void* body;
    task->get_resp()->get_parsed_body(&body, size);
}

int main()
{
    // 1. 创建匿名定时任务：定时1s钟
    WFTimerTask* timer = WFTaskFactory::create_timer_task(3, 0, [](WFTimerTask*) {
        cout << "timer task complete(3s)." << endl;
    });
    // 2. 创建HTTP任务：获取baidu首页
    WFHttpTask* fetch_baidu = WFTaskFactory::create_http_task("https://www.baidu.com", 3, 3, http_callback);
    size_t size1 = 0;
    fetch_baidu->user_data = &size1;
    // 3. 创建HTTP任务：获取sogou首页
    WFHttpTask* fetch_sogou = WFTaskFactory::create_http_task("https://www.sogou.com", 3, 3, http_callback);
    size_t size2 = 0;
    fetch_sogou->user_data = &size2;
    // 4. 创建计算任务：打印结果
    WFGoTask* display = WFTaskFactory::create_go_task("display", [&]() {
        cout << "百度首页大小：" << size1 << endl;
        cout << "搜狗首页大小：" << size2 << endl;
    });
    // 5. 创建图任务：管理任务之间的依赖关系
    WFFacilities::WaitGroup waitGroup(1);
    WFGraphTask* graph = WFTaskFactory::create_graph_task([&waitGroup](WFGraphTask*) {
        cout << "Graph task complete. Wakeup main thread." << endl;
        waitGroup.done();
    });
    // 将各类任务包装成图任务的节点(node)
    WFGraphNode& a = graph->create_graph_node(timer);
    WFGraphNode& b = graph->create_graph_node(fetch_baidu);
    WFGraphNode& c = graph->create_graph_node(fetch_sogou);
    WFGraphNode& d = graph->create_graph_node(display);
    // 指定任务之间的依赖关系
    a --> b;
    a --> c;
    b --> d;
    c --> d;
    // 6. 启动图任务
    graph->start();
    // 7. 主线程等待图任务结束
    waitGroup.wait();
    cout << "主线程退出" << endl;
    return 0;
}
