/**
 * [[MYSQL任务]]
 *
 * [URL]
 * mysql://username:password@host:port/dbname?character_set=charset&character_set_results=charset
 * port: 默认为3306
 * dbname: 如果只操作一个数据库，建议填写
 *
 * [回调函数的流程]
 *
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
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

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
        unsigned long long rows = cursor.get_affected_rows(); // 获取受影响的行数
        cout << rows << " rows affected" << endl;
        unsigned long long id = cursor.get_insert_id(); // 获取插入记录的id
        cout << "insert id: " << id << endl;
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
    string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ('peanut', '1234', 'RAND_SALT');";
    req->set_query(sql);
    // 3. 添加到任务序列（串行流）中
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
