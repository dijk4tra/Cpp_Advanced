#include "ServiceCommon.h"

#include <cstdlib>
#include <iostream>
#include <workflow/WFFacilities.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/Workflow.h>

using namespace std;
using namespace protocol;

// MySQL 地址
const string DatabaseURL = "mysql://root:123456@localhost/CloudDisk";

// MySQL 查询失败时最多重试 3 次。
const int RetryMax = 3;

unsigned short get_env_port(const char* name, unsigned short default_port)
{
    // getenv() 从当前进程环境变量中读取字符串。
    // 如果 run.sh 没有设置这个变量，getenv() 会返回 nullptr。
    const char* value = getenv(name);

    // 环境变量不存在或为空字符串时，直接使用默认端口。
    if (value == nullptr || string(value).empty()) {
        return default_port;
    }

    // strtol() 把字符串转成长整数。
    // end 指向“数字解析结束的位置”，用于判断字符串是否完全是数字。
    char* end = nullptr;
    long port = strtol(value, &end, 10);

    // 合法端口范围是 1-65535。
    // 如果字符串里有非数字字符，或者数字越界，也回退到默认端口。
    if (*end != '\0' || port <= 0 || port > 65535) {
        return default_port;
    }

    // 前面已经确认范围合法，这里可以安全转成 unsigned short。
    return static_cast<unsigned short>(port);
}

string escape_sql(const string& s)
{
    // result 用来保存转义后的字符串。
    // reserve() 先预留原字符串长度，减少小字符串追加时的内存重新分配。
    string result;
    result.reserve(s.size());

    // 逐字符扫描输入字符串。
    // 本学习项目只处理单引号和反斜线两个最常见问题字符。
    for (char ch : s) {
        // SQL 字符串用单引号包起来。
        // 如果用户名里有单引号，就需要写成 \'，避免提前结束字符串。
        if (ch == '\'') {
            result += "\\'";
        // 反斜线本身是转义字符。
        // 如果用户输入反斜线，需要写成 \\，避免影响后面的字符。
        } else if (ch == '\\') {
            result += "\\\\";
        // 普通字符直接追加。
        } else {
            result += ch;
        }
    }

    // 返回已经适合拼接进 SQL 单引号字符串的内容。
    return result;
}

void set_result(cloud::disk::CommonResult* result, int code, const string& message)
{
    // protobuf 的子 message 通过 mutable_result() 拿到可写指针。
    // 这里的 result 就是那个可写指针。
    result->set_code(code);

    // message 是给 API Gateway 转成 HTTP JSON 后返回给前端看的文本。
    result->set_message(message);
}

bool run_mysql_query(const string& sql,
                     const function<void(MySQLResultCursor&)>& handler)
{
    // ok 表示这次 MySQL 任务是否成功走到了业务处理阶段。
    // 默认 false，只有网络层和 MySQL 返回包都正常时才改成 true。
    bool ok = false;

    // WaitGroup 用来把异步 MySQL 任务“等成”当前函数里的顺序流程。
    // 初始值 1 表示还有一个异步任务没有完成。
    WFFacilities::WaitGroup wait_group(1);

    // 创建 Workflow MySQL 任务。
    // 回调会在 MySQL 响应返回后执行。
    WFMySQLTask* task = WFTaskFactory::create_mysql_task(
        DatabaseURL,
        RetryMax,
        [&](WFMySQLTask* task) {
            // 第一步：判断网络任务是否成功。
            // 如果 MySQL 服务不可达、连接失败、超时，这里会不是 WFT_STATE_SUCCESS。
            int state = task->get_state();
            if (state != WFT_STATE_SUCCESS) {
                cerr << "[MySQL task FAILED] "
                     << WFGlobal::get_error_string(state, task->get_error())
                     << endl;
                return;
            }

            // 第二步：取出 MySQL 响应对象。
            // 响应对象中保存了 MySQL 返回包和后续结果集。
            MySQLResponse* mysql_resp = task->get_resp();

            // MYSQL_PACKET_ERROR 表示 MySQL 服务器返回了错误包。
            // 常见原因包括 SQL 语法错误、表不存在、唯一键冲突等。
            if (mysql_resp->get_packet_type() == MYSQL_PACKET_ERROR) {
                cerr << "[MySQL packet ERROR] code="
                     << mysql_resp->get_error_code()
                     << ", msg=" << mysql_resp->get_error_msg()
                     << endl;
                return;
            }

            // MySQLResultCursor 是 Workflow 提供的结果游标。
            // SELECT 用它 fetch_row() 读取多行；
            // INSERT 用它 get_insert_id() 读取新插入 id。
            MySQLResultCursor cursor(mysql_resp);

            // 把 cursor 交给调用方读取。
            // 注意 cursor 不能保存到回调外长期使用，所以 handler 里要立即把需要的数据拷贝出来。
            handler(cursor);

            // handler 能执行到这里，说明网络层和 MySQL 返回包都没有失败。
            ok = true;
        });

    // 把 SQL 写入 MySQL 请求。
    task->get_req()->set_query(sql);

    // 用 SeriesWork 包住单个 MySQL task。
    // SeriesWork 完成时调用 wait_group.done()，让下面的 wait() 返回。
    SeriesWork* series = Workflow::create_series_work(
        task,
        [&wait_group](const SeriesWork*) {
            wait_group.done();
        });

    // 启动异步任务。
    series->start();

    // 等待 MySQL 任务完成。
    // 当前线程会在这里阻塞，但只阻塞当前 srpc 服务端处理流程。
    wait_group.wait();

    // 返回这次 SQL 是否顺利执行到 handler。
    return ok;
}
