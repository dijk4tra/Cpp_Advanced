#include <cstddef>
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFGraphTask.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;

void http_callback(WFHttpTask* httpTask)
{
    int state = httpTask->get_state();
    if (state != WFT_STATE_SUCCESS) {
       return;
    }
    size_t* size = static_cast<size_t*>(httpTask->user_data);
    const void* body;
    httpTask->get_resp()->get_parsed_body(&body, size);
}


int main(int argc, char* argv[])
{
    // 1. 创建4个任务
    WFTimerTask* timer = WFTaskFactory::create_timer_task(3, 0, [](WFTimerTask*) {
        cout << "timer task finished(3s)." << endl;
    });

    WFHttpTask* fetch_baidu = WFTaskFactory::create_http_task("http://www.baidu.com", 3, 3, http_callback);
    size_t size1;
    fetch_baidu->user_data = &size1;

    WFHttpTask* fetch_sougou = WFTaskFactory::create_http_task("http://www.sougou.com", 3, 3, http_callback);
    size_t size2;
    fetch_sougou->user_data = &size2;

    WFGoTask* go = WFTaskFactory::create_go_task("go", [&]() {
        cout << "百度首页的大小: " << size1 << endl;
        cout << "搜狗首页的大小: " << size2 << endl;
    });

    // 2. 构建DAG图任务
    // 空图, 没有任何节点
    WFGraphTask* graph = WFTaskFactory::create_graph_task([](const WFGraphTask*) {
        cout << "DAG graph task finished." << endl;
    });

    // 添加四个孤立的节点
    WFGraphNode& a = graph->create_graph_node(timer);
    WFGraphNode& b = graph->create_graph_node(fetch_baidu);
    WFGraphNode& c = graph->create_graph_node(fetch_sougou);
    WFGraphNode& d = graph->create_graph_node(go);

    // 定义节点之间的依赖关系
    a --> b;
    b --> d;
    a --> c;
    c --> d;

    // 3. 启动DAG图任务(拓扑排序)
    WFFacilities::WaitGroup waitGroup(1);

    Workflow::start_series_work(graph, [&waitGroup](const SeriesWork*) {
        waitGroup.done();
    });

    // 4. 主线程等待图任务结束
    waitGroup.wait();
    return 0;
}
