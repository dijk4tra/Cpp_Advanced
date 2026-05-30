/**
 * 接下来，我们来看一个更加具体的例子: 类似 wget url... -q -O -
 *
 * 程序从命令行读取多个 http URL (以空格分隔)，并行抓取这些网页，
 * 并按照输入顺序将抓取结果打印到标准输出。如果抓取失败，则打印出错原因
 */
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

// state, error：出错原因
// resp：抓取的网页
struct SeriesContext {
    string url;
    int state;
    int error;
    HttpResponse resp;
};

void http_callback(WFHttpTask* task)
{
    // 在HTTP任务的回调函数中，设置SeriesContext的state, error和resp。
    SeriesContext* ctx = static_cast<SeriesContext*>(series_of(task)->get_context());
    ctx->state = task->get_state();
    ctx->error = task->get_error();
    // http_callback 执行完后，HTTP任务就会被销毁，
    // 其中的 HttpRequest 和 HttpResponse 会一并销毁！
    // 所以我们这里可以用移动语义，避免没必要的复制
    ctx->resp = move(*task->get_resp());
}

void parallel_callback(const ParallelWork* parallel)
{
    for (int i = 0; i < parallel->size(); ++i) {
        const SeriesWork* series = parallel->series_at(i);
        SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
        // 打印URL
        cout << ctx->url << ": " << endl;
        if (ctx->state == WFT_STATE_SUCCESS) {
            // 成功，打印网页内容
            const void* body;
            size_t size;
            ctx->resp.get_parsed_body(&body, &size);
            cout << static_cast<const char*>(body) << "\n\n";
        } else {
            // 失败，打印出错信息
            cout << WFGlobal::get_error_string(ctx->state, ctx->error) << endl;
        }
        // 上下文使用完毕，不要忘记销毁！
        delete ctx;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        cout << "Usage: " << argv[0] << "<url>..." << endl;
        exit(1);
    }
    // 1. 创建空的并行任务
    ParallelWork* parallel = Workflow::create_parallel_work(parallel_callback);
    // 2. 读取用户输入的URL，创建序列，并添加到ParallelWork中。
    for (int i = 1; i < argc; ++i) {
        // 创建HTTP任务
        WFHttpTask* task = WFTaskFactory::create_http_task(argv[i], 3, 3, http_callback);
        // 设置请求
        HttpRequest* req = task->get_req();
        req->add_header_pair("Accept", "*/*");
        req->add_header_pair("User-Agent", "paralle_wget (linux-gnu)");
        req->add_header_pair("Connection", "close");
        // 创建序列，并将HTTP任务添加到序列中
        SeriesWork* series = Workflow::create_series_work(task, nullptr);

        // 题目要求按输入URL的顺序打印对应的网页，但ParallelWork中的序列是并发执行的。
        // 因此，我们不能在序列的回调函数中直接输出抓取的网页，
        // 而应该在ParallelWork的回调函数中，按输入URL的顺序依次输出。
        // (回顾：ParallelWork的回调函数会在所有序列执行完后才执行)
        // 所以我们需要在序列的上下文中保存抓取的结果。
        SeriesContext* ctx = new SeriesContext;
        // 此时，HTTP任务还没执行, 我们只需要设置ctx->url即可
        // ctx->state, ctx->error, ctx->resp 需要在HTTP任务的回调函数中设置
        ctx->url = argv[i];
        series->set_context(ctx);
        parallel->add_series(series);
    }
    // 3. 将ParallelWork放入一个序列中执行（ParallelWork也是一个任务）。
    // 在Workflow中，这是一种更加规范的写法。
    WFFacilities::WaitGroup waitGroup(1);
    Workflow::start_series_work(parallel, [&waitGroup](const SeriesWork*) {
        waitGroup.done();
    });
    // 4. 主线程等待ParallelWork结束
    waitGroup.wait();
    return 0;
}
