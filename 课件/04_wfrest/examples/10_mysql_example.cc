/**
 * 对于MySQL这种常见的任务，我们当然也可以使用SeriesHandler进行集成：
 *     WFMySQLTask* task = WFTaskFactory::create_mysql_task(url, retry_max, callback);
 *     ...
 *     series->push_back(task);
 * 但wfrest为了方便程序员的使用，对这类常见的任务进行了封装
 * 我们可以使用 resp->MySQL(...) 完成上面的工作
 */
#include <iostream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <wfrest/HttpServer.h>
#include <workflow/MySQLResult.h>
#include <workflow/WFFacilities.h>

using namespace std;
using namespace wfrest;
using namespace protocol;
using json = nlohmann::json;

static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int signum)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer server;

    server.GET("/mysql1", [](const HttpReq* req, HttpResp* resp) {
        // 原理：
        // 1. 创建MySQL任务，
        // 2. push_back到SeriesWork中，
        // 3. 根据MySQL返回的结果集，构建JSON，并返回给客户端
        resp->MySQL("mysql://root:1234@localhost", "SHOW DATABASES");
    });

    // 执行多条sql语句
    server.GET("/mysql2", [](const HttpReq* req, HttpResp* resp) {
        string url = "mysql://root:1234@localhost/test";
        string sql = "SHOW DATABASES; SELECT * FROM tbl_user";
        resp->MySQL(url, sql);
    });

    /* 如果我们对默认的返回消息不满意，也可以自定义
     * using MySQLFunc = std::function<void(protocol::MySQLResultCursor* cursor)>;
     */
    server.GET("/mysql3", [](const HttpReq* req, HttpResp* resp) {
        string url = "mysql://root:1234@localhost";
        string sql = "SHOW DATABASES";
        resp->MySQL(url, sql, [resp](MySQLResultCursor* cursor) {
            // 自己构建返回消息
            json result = json::array();
            vector<MySQLCell> record;
            while (cursor->fetch_row(record)) {
                result.push_back(record[0].as_string());
            }
            resp->Json(result.dump());
        });
    });

    if (server.start(8888) == 0) {
        server.list_routes();
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
        exit(1);
    }
    return 0;
}
