#include "ServiceCommon.h"

#include <cstdlib>
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

const string DatabaseURL = "mysql://root:123456@localhost/CloudDisk";
const int RetryMax = 3;

unsigned short get_env_port(const char* name, unsigned short default_port)
{
    const char* value = getenv(name);

    if (value == nullptr || string(value).empty()) {
        return default_port;
    }

    char* end = nullptr;
    long port = strtol(value, &end, 10);

    if (*end != '\0' || port <= 0 || port > 65535) {
        return default_port;
    }

    return static_cast<unsigned short>(port);
}

string escape_sql(const string& s)
{
    string result;
    result.reserve(s.size());

    // 当前项目仍拼接 SQL，这里只处理会破坏字符串字面量的字符。
    for (char ch : s) {
        if (ch == '\'') {
            result += "\\'";
        } else if (ch == '\\') {
            result += "\\\\";
        } else {
            result += ch;
        }
    }

    return result;
}

void set_result(cloud::disk::CommonResult* result, int code, const string& message)
{
    result->set_code(code);
    result->set_message(message);
}

bool run_mysql_query(const string& sql,
                     const function<void(MySQLResultCursor&)>& handler)
{
    bool ok = false;
    WFFacilities::WaitGroup wait_group(1);

    WFMySQLTask* task = WFTaskFactory::create_mysql_task(
        DatabaseURL,
        RetryMax,
        [&](WFMySQLTask* task) {
            int state = task->get_state();
            if (state != WFT_STATE_SUCCESS) {
                cerr << "[MySQL task FAILED] "
                     << WFGlobal::get_error_string(state, task->get_error())
                     << endl;
                return;
            }

            MySQLResponse* mysql_resp = task->get_resp();

            if (mysql_resp->get_packet_type() == MYSQL_PACKET_ERROR) {
                cerr << "[MySQL packet ERROR] code="
                     << mysql_resp->get_error_code()
                     << ", msg=" << mysql_resp->get_error_msg()
                     << endl;
                return;
            }

            MySQLResultCursor cursor(mysql_resp);

            // cursor 生命周期只在回调内有效，handler 内需要立即拷贝所需数据。
            handler(cursor);
            ok = true;
        });

    task->get_req()->set_query(sql);

    SeriesWork* series = Workflow::create_series_work(
        task,
        [&wait_group](const SeriesWork*) {
            wait_group.done();
        });

    series->start();
    wait_group.wait();

    return ok;
}
