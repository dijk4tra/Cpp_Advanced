#include <iostream>
#include <workflow/HttpMessage.h>
#include <workflow/MySQLMessage.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/WFFacilities.h>
#include <workflow/Workflow.h>
#include <workflow/MySQLResult.h>

using namespace std;
using namespace protocol;

// 整个 SeriesWork 共用的上下文
// 用来在 HTTP 任务、MySQL 任务、series callback 之间传递数据
struct SeriesContext {
    string url; // 网页 URL
    size_t size; // HTTP 响应体大小
    bool success; // 整个序列是否成功
};

// MySQL 任务的回调函数
// 当 MySQL 任务执行结束后，会自动调用这个函数
void mysql_callback(WFMySQLTask* mysqlTask)
{
    // 1. 判断 MySQL 任务的状态
    // 这里判断的是网络连接、任务调度、超时等层面的错误
    int state = mysqlTask->get_state();
    if (state != WFT_STATE_SUCCESS){
        cerr << "MySQL任务失败: "
             << WFGlobal::get_error_string(state, mysqlTask->get_error())
             << endl;
        return;
    }

    // 2. 判断 MySQL 服务端返回的包类型
    MySQLResponse* resp = mysqlTask->get_resp();
    if (resp->get_packet_type() != MYSQL_PACKET_OK){
        cerr << resp->get_error_code() << " "
             << resp->get_error_msg() << endl;
        return;
    }

    // 3. SQL 执行成功，修改序列上下文
    SeriesWork* series = series_of(mysqlTask);
    SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
    ctx->success = true;

    // 4. 读取 INSERT 后生成的自增 ID
    MySQLResultCursor cursor(resp);
    cout << "MySQL任务完成!" << endl;
    cout << "插入记录: id = " << cursor.get_insert_id() << endl;
}

// HTTP 任务的回调函数
// 当 HTTP 请求完成后，会自动调用这个函数
void http_callback(WFHttpTask* httpTask)
{
    // 1. 判断 HTTP 任务的状态
    // 这里判断的是 DNS、网络连接、超时、任务调度等层面的错误
    int state = httpTask->get_state();
    if (state != WFT_STATE_SUCCESS){
        cerr << WFGlobal::get_error_string(state, httpTask->get_error())<< endl;
        return;
    }

    // 2. 获取当前任务所在的 SeriesWork 和上下文
    SeriesWork* series = series_of(httpTask);
    SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());

    // 3. 获取 HTTP 响应
    HttpResponse* resp = httpTask->get_resp();

    // 4. 判断 HTTP 状态码
    // 注意：WFT_STATE_SUCCESS 只说明网络请求成功完成，不代表服务器返回的是正常页面
    // 这里只把 2xx 状态码当作成功网页处理
    const char* status_code = resp->get_status_code();
    if (status_code == nullptr || status_code[0] != '2') {
        cerr << "HTTP请求失败，状态码: "
             << (status_code ? status_code : "unknown")
             << endl;
        return;
    }

    // 5. 获取响应体
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size);

    // 保存网页大小到上下文
    ctx->size = size; // 设置 ctx->size

    // [解析响应体]
    // 6. 创建 MySQL 任务
    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
        "mysql://root:123456@localhost:3306/demo",
        3, // retry_max
        mysql_callback
    );

    // 7. 拼接 SQL
    // string url = static_cast<const char*>(httpTask->user_data);
    string url = ctx->url;
    string sql = "INSERT INTO tbl_webpage (url, size) VALUES ('"
        + url + "', "
        + std::to_string(size) + ")";
    cout << "[SQL] " << sql << endl;

    // 8. 设置 MySQL 请求的 SQL
    MySQLRequest* mysql_req = mysqlTask->get_req();
    mysql_req->set_query(sql);

    // 9. 把 MySQL 任务追加到当前 HTTP 任务所在的 SeriesWork 序列
    series->push_back(mysqlTask);
}

int main(int argc, char *argv[]) {
    // 1. 校验命令行参数
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <URL>" << endl;
        return 1;
    }

    // 2. 创建 HTTP 任务
    WFHttpTask* httpTask = WFTaskFactory::create_http_task(
        argv[1],
        3,
        3,
        http_callback
    );

    // httpTask->user_data = argv[1]; // 可以使用 user_data 传递 URL, 也可使用 SeriesContext 上下文传递 URL

    // 3. 设置 HTTP 请求头
    HttpRequest* req = httpTask->get_req();
    req->add_header_pair("Accept", "*/*");
    req->add_header_pair("User-Agent", "wget/1.14 (linux-gnu)");
    req->add_header_pair("Connection", "close");

    // 4. 设置 HTTP 响应体大小限制和接收超时时间
    HttpResponse* resp = httpTask->get_resp();
    resp->set_size_limit(20 * 1024 * 1024); // 20MB
    httpTask->set_receive_timeout(30 * 1000); // 30s

    // 5. 创建 WaitGroup
    WFFacilities::WaitGroup waitGroup(1);

    // 6. 创建串行任务流 SeriesWork
    // 当前序列一开始只有 httpTask
    // 在 http_callback 中，会继续把 mysqlTask 追加到这个序列后面
    // 最终执行顺序: httpTask -> mysqlTask -> series callback
    SeriesWork* series = Workflow::create_series_work(httpTask, [&waitGroup](const SeriesWork* series) {  //这里需要是引用捕获!不能值捕获(WaitGroup为了防止使用时出错,已经删除了拷贝构造函数)
        SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
        if (ctx->success) {
            cout << "success! " << endl;
        } else {
            cout << "failed! " << endl;
        }
        delete ctx;
        waitGroup.done();
    });

    // 设置序列的上下文
    SeriesContext* ctx = new SeriesContext{ argv[1], 0, false };
    series->set_context(ctx);

    // 7. 启动序列
    series->start();

    // 8. 主线程等待异步任务结束
    waitGroup.wait();
}
