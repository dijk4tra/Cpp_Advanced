#include <iostream>
#include <ostream>
#include <string>
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

void mysql_callback(WFMySQLTask* task)
{
    // 1. 判断任务的状态
    int state = task->get_state();
    if (state != WFT_STATE_SUCCESS){
        cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
        waitGroup.done();
        return;
    }

    // 2. 任务成功, 收到响应: 判断返回包的类型
    MySQLResponse* resp = task->get_resp();
    if (resp->get_packet_type() == MYSQL_PACKET_ERROR){
        cerr << "error_code: " << resp->get_error_code()
             << ", error_msg: " << resp->get_error_msg() << endl;
        waitGroup.done();
        return;
    }

    // 3. DML: 处理结果集
    MySQLResultCursor cursor(resp);
    if (cursor.get_cursor_status() == MYSQL_STATUS_OK) { // DML操作成功
        unsigned long long rows = cursor.get_affected_rows(); // 获取受影响的行数
        cout << rows << "row affected" << endl;
        unsigned long long id = cursor.get_insert_id(); // 获取插入记录的id
        cout << "insert id: " << id << endl;
    }

    waitGroup.done();
}


int main()
{
    // 1. 创建MySQL任务
    WFMySQLTask* task = WFTaskFactory::create_mysql_task(
        "mysql://root:123456@localhost:3306/demo",
        3,
        mysql_callback
    );

    // 2. 设置任务: 指定SQL语句
    protocol::MySQLRequest* req = task->get_req();
    string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ('lili', '123456', 'very high')";
    req->set_query(sql); // 设置要执行的SQL语句

    // 3. 启动任务
    task->start();

    // 4. 等待任务执行完成
    waitGroup.wait(); // 让当前线程阻塞，直到waitGroup的值为0

    return 0;
}
