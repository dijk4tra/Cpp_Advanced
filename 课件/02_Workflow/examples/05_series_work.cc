/**
 * 读取用户输入的URL，抓取网页，[解析网页]，并存入数据库。
 */
#include <iostream>
#include <netdb.h>
#include <stdlib.h>
#include <string>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

#define RETRY_MAX 3

struct SeriesContext {
    string url;
    size_t size; // 网页的大小
    bool success; // 序列是否成功
};

void mysql_callback(WFMySQLTask* task)
{
    // 1. 判断任务是否成功
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        return;
    }
    // 2. 判断返回包的类型
    MySQLResponse* resp = task->get_resp();
    if (resp->get_packet_type() == MYSQL_PACKET_ERROR) {
        cerr << "error_code: " << resp->get_error_code()
             << ", error_msg: " << resp->get_error_msg() << endl;
        return;
    }
    // 3. 处理结果集
    MySQLResultCursor cursor(resp);
    if (cursor.get_cursor_status() == MYSQL_STATUS_OK) { // DML操作成功
        cout << "插入记录成功，新纪录的id为：" << cursor.get_insert_id() << endl;
        // 更改序列的上下文
        SeriesContext* ctx = static_cast<SeriesContext*>(series_of(task)->get_context());
        ctx->success = true;
    }
}

void http_callback(WFHttpTask* task)
{
    // 1. 检查任务的状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        return;
    }
    // 2. 获取任务所在的序列和序列的上下文
    SeriesWork* series = series_of(task);
    SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
    // 3. 获取响应体
    HttpResponse* resp = task->get_resp();
    const void* body;
    size_t size;
    resp->get_parsed_body(&body, &size);
    ctx->size = size;
    // 4. [[解析网页]]
    cout << "网页内容：\n"
         << static_cast<const char*>(body) << endl;
    // 5. 创建MySQL任务
    string db = "mysql://root:1234@127.0.0.1/demo";
    WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(db, RETRY_MAX, mysql_callback);
    // 设置请求
    MySQLRequest* req = mysqlTask->get_req();
    // 拼接SQL语句
    string sql = "INSERT INTO tbl_webpage (url, size) VALUES ('"
        + ctx->url + "', "
        + std::to_string(size) + ");";
    cout << "[SQL]: " << sql << endl; // 打印调试信息
    req->set_query(sql);
    // 将MySQL任务添加到序列中
    series->push_back(mysqlTask);
}

int main(int argc, char* argv[])
{
    // 1. 校验命令行参数
    if (argc != 2) {
        cerr << "USAGE: " << argv[0] << " <URL>" << endl;
        exit(1);
    }
    // 2. 创建HTTP任务
    WFHttpTask* httpTask = WFTaskFactory::create_http_task(argv[1], 3, RETRY_MAX, http_callback);
    // 设置请求
    HttpRequest* req = httpTask->get_req();
    req->add_header_pair("Accept", "*/*");
    req->add_header_pair("User-Agent", "wget/1.14 (linux-gnu)");
    req->add_header_pair("Connection", "close");
    // 设置响应的最大大小：20M
    httpTask->get_resp()->set_size_limit(20 * 1024 * 1024);
    // 设置响应的超时时间: 30s
    httpTask->set_receive_timeout(30 * 1000);
    // 3. 创建SeriesWork（串行任务流，序列）
    WFFacilities::WaitGroup waitGroup(1);
    SeriesWork* series = Workflow::create_series_work(httpTask, [&waitGroup](const SeriesWork* series) {
        // 获取序列的上下文
        SeriesContext* ctx = static_cast<SeriesContext*>(series->get_context());
        if (ctx->success) {
            cout << "SeriesWork SUCCESS!" << endl;
        } else {
            cerr << "SeriesWork FAILURE!" << endl;
        }
        // 整个序列结束
        delete ctx; // 释放上下文
        waitGroup.done();
    });
    // 4. 设置序列的上下文
    SeriesContext* ctx = new SeriesContext { argv[1], 0, false };
    series->set_context(ctx);
    // 5. 启动序列
    series->start();
    // 6. 主线程等待序列结束
    waitGroup.wait();
    return 0;
}
