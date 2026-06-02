#include <cmath>
#include <iostream>
#include <signal.h>
#include <vector>
#include <workflow/MySQLMessage.h>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTask.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/mysql_types.h>

using namespace std;
using namespace protocol;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

// 设计函数参数时需要考虑：
// 1. 值传递 or 引用传递
// 2. 如果是引用传递，要不要加const
void display_cell(const MySQLCell& cell)
{
    // 不同系统的类型适配
    if (cell.is_null()) { // MySQL: NULL(没有数据)
        cout << "(NULL)";
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
    // 1. 判断任务的状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS)
    {
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        return;
    }

    // 2. 判断最后一个返回包的类型
    MySQLResponse* resp = task->get_resp();
    if (resp->get_packet_type() == MYSQL_PACKET_ERROR) {
        // SQL语句执行失败：语法错误或者违反了约束条件
        cerr << "error_code: " << resp->get_error_code()
             << ", error_msg: " << resp->get_error_msg() << endl;
        return;
    }

    // 3. 处理结果集(DQL)
    MySQLResultCursor cursor(resp);
    if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT) { // 指向DQL操作的结果集
        cout << "fields: " << cursor.get_field_count() << endl;  // 获取字段个数
        cout << "rows: " << cursor.get_rows_count() << endl;     // 获取行数

        // 遍历并输出所有的记录
        vector<MySQLCell> record;
        while (cursor.fetch_row(record)) {
            // 成功获取一条记录
            for (const MySQLCell& cell : record) {
                display_cell(cell);
                cout << "\t";
            }
            cout << endl;
        }
    }
}

int main()
{
    // 注册信号
    signal(SIGINT, sig_handler);

    // 1. 创建MySQL任务
    WFMySQLTask* task = WFTaskFactory::create_mysql_task(
        "mysql://root:123456@localhost:3306/demo",
        3,
        mysql_callback
    );

    // 2. 设置任务参数(要执行的SQL语句)
    string sql = "SELECT * FROM tbl_user";
    MySQLRequest* req = task->get_req();
    req->set_query(sql);

    // 3. 启动任务
    task->start();

    // 4. 等待任务完成
    waitGroup.wait();

    return 0;
}
