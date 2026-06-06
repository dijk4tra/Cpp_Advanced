#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

struct SeriesContext
{
    string url;
    int state;
    int error;
    HttpResponse resp;
};

void http_callback(WFHttpTask* httpTask)
{
   SeriesWork* series = series_of(httpTask);
   SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());

   ctx->state = httpTask->get_state();
   ctx->error = httpTask->get_error();
   // http_callback执行完后，HTTP任务就会被销毁，
   // 其中的HttpRequest和HttpResponse会一并销毁！
   // 所以这里可以用移动语义，避免没必要的复制
   // 先解引用, 再用移动语义
   ctx->resp = std::move(*httpTask->get_resp()); // 这里WorkFlow把拷贝构造禁用掉了
}

// 按用户输入URI的顺序输出网页
void parallel_callback(const ParallelWork* parallelWork)
{
    for (int i = 0; i < parallelWork->size(); ++i){
        const SeriesWork* series = parallelWork->series_at(i);
        // 获取序列的上下文
        SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
        cout << ctx->url << ": " << endl;
        if (ctx->state != WFT_STATE_SUCCESS) {
            cout << WFGlobal::get_error_string(ctx->state, ctx->error) << endl;
        } else {
            const void* body;
            size_t size;
            ctx->resp.get_parsed_body(&body, &size);
            cout << static_cast<const char*>(body) << endl;
        }

        delete ctx;
    }
}

int main(int argc, char* argv[])
{
    // 0. 命令行参数校验
    if (argc < 2){
        cerr << "Usage: " << argv[0] << " <URI>" << endl;
        exit(1);
    }

    // 1. 创建空的并行任务
    ParallelWork* parallelWork = Workflow::create_parallel_work(parallel_callback);

    // 2. 读取用户输入的URL，创建序列，并添加到ParallelWork 中
    for (int i = 1; i < argc; ++i) {
        // 创建HTTP任务
        WFHttpTask* httpTask = WFTaskFactory::create_http_task(argv[i], 3, 3, http_callback);

        // 设置请求
        HttpRequest* req = httpTask->get_req();
        req->add_header_pair("Accept", "*/*");
        req->add_header_pair("User-Agent", "parallel_wget (linux-gnu)");
        req->add_header_pair("Connection", "close");

        // 创建序列, 将HTTP任务添加到序列中
        SeriesWork* series = Workflow::create_series_work(httpTask, nullptr);

        // 需要按输入URL的顺序打印对应的网页，但ParallelWork中的序列是并发执行的
        // 所以不能在序列的回调函数中直接输出抓取的网页
        // 而应该在ParallelWork的回调函数中，按输入URL的顺序依次输出
        // 因此需要在序列的上下文中保存抓取的结果
        SeriesContext* ctx = new SeriesContext;
        // 此时，HTTP任务还没执行, 只需要设置ctx->url即可
        ctx->url = argv[i];
        series->set_context(ctx);

        // 按用户输入URI的顺序添加SeriesWork至parallelWork
        parallelWork->add_series(series);
    }

    // 3. 启动ParallelWork
    WFFacilities::WaitGroup waitGroup(1);
    Workflow::start_series_work(parallelWork, [&waitGroup](const SeriesWork*) {
        waitGroup.done();
    });

    // 4. 主线程阻塞, 等待ParallelWork完成
    waitGroup.wait();

    return 0;
}
