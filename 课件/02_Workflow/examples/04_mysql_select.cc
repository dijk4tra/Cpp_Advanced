/**
 * [[MYSQL任务]]
 *
 * [URL]
 * mysql://username:password@host:port/dbname?character_set=charset&character_set_results=charset
 * port: 默认为3306
 * dbname: 如果只操作一个数据库，建议填写
 *
 * [回调函数的流程]
 * 1. 判断网络任务是否成功 (是否收到响应)
 *    task->get_state() != WFT_STATE_SUCCESS
 * 2. 查看最后一条SQL语句的返回包类型: resp->get_packet_type()
 *    常见类型为：
 *      MYSQL_PACKET_OK：成功，可以用cursor遍历结果；
 *      MYSQL_PACKET_EOF：成功，可以用cursor遍历结果；
 *      MYSQL_PACKET_ERROR：失败或部分失败，成功的部分可以用cursor遍历结果；
 * 3. 判断结果集的类型：cusor.get_cursor_status()
 *      MYSQL_STATUS_OK: DML操作                --> get_insert_id(), get_affected_rows(), ...
 *      MYSQL_STATUS_GET_RESULT: DQL操作        --> fetch_fields(),  fetch_row(), ...
 */

#include <iostream>
#include <string>
#include <vector>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

// MySQL数据类型和C++数据类型的适配
void display_mysql_cell(const MySQLCell& cell)
{
    // 将丑陋的代码封装起来，避免影响其它代码
    if (cell.is_null()) {
        cout << "(null)";
    } else if (cell.is_int()) {
        cout << cell.as_int();
    } else if (cell.is_ulonglong()) {
        cout << cell.as_ulonglong();
    } else if (cell.is_float()) {
        cout << cell.as_float();
    } else if (cell.is_double()) {
        cout << cell.as_double();
    } else if (cell.is_string()) {
        cout << cell.as_string();
    } else if (cell.is_date()) {
        cout << cell.as_date();
    } else if (cell.is_time()) {
        cout << cell.as_time();
    } else if (cell.is_datetime()) {
        cout << cell.as_datetime();
    }
}

void mysql_callback(WFMySQLTask* task)
{
    // 1. 判断任务是否成功
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS) {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        return;
    }
    // 2. 判断MySQL返回包的类型
    MySQLResponse* resp = task->get_resp();
    if (resp->get_packet_type() == MYSQL_PACKET_ERROR) {
        cerr << "error_code: " << resp->get_error_code()
             << ", error_msg: " << resp->get_error_msg() << endl;
        return;
    }
    // 3. 处理结果集
    MySQLResultCursor cursor(resp);
    if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT) { // DQL操作执行成功
        // 使用MySQLResultCursor遍历结果集
        vector<MySQLCell> record;
        while (cursor.fetch_row(record)) { // 获取下一条记录
            for (const auto& cell : record) {
                display_mysql_cell(cell);
                cout << " ";
            }
            cout << endl;
        }
    }
}

int main()
{
    // 1. 创建任务
    string url = "mysql://root:1234@127.0.0.1/demo";
    int retry_max = 3;
    WFMySQLTask* task = WFTaskFactory::create_mysql_task(url, retry_max, mysql_callback);
    // 2. 设置任务：指定SQL语句
    MySQLRequest* req = task->get_req();
    string sql = "SELECT * FROM tbl_user";
    req->set_query(sql);
    // 3. 将任务添加到序列（串行流）中
    WFFacilities::WaitGroup waitGroup(1);
    SeriesWork* series = Workflow::create_series_work(task, [&waitGroup](const SeriesWork*) {
        waitGroup.done();
    });
    // 4. 启动序列
    series->start();
    // 5. 主线程等待整个序列执行完毕
    waitGroup.wait();
    return 0;
}
