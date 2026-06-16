#pragma once

#include "../../rpc_gen/cloud_disk.pb.h"

#include <functional>
#include <string>
#include <workflow/MySQLResult.h>

/*
    ServiceCommon.h 放微服务端共用的小工具。

    本项目是学习项目，所以这里没有做复杂的数据库连接池、
    配置中心、日志框架，只保留第四期改造真正需要的公共能力。
*/

/*
    数据库连接 URL。

    第三期代码直接在 CloudDiskServer.cc 中写死这个 URL。
    第四期拆成多个服务后，AuthService/UserService/FileMetaService
    都要访问同一个学习用 MySQL 数据库，所以把 URL 放到公共头文件中。
*/
extern const std::string DatabaseURL;

/*
    Workflow MySQL 任务的最大重试次数。

    保持和第三期 CloudDiskServer.cc 中的 RetryMax=3 一致。
*/
extern const int RetryMax;

/*
    从环境变量读取端口。

    参数说明：
    - name：环境变量名，例如 "AUTH_SERVICE_PORT"。
    - default_port：环境变量不存在时使用的默认端口。

    返回值：
    - 如果环境变量存在且是合法数字，返回环境变量中的端口。
    - 否则返回 default_port。
*/
unsigned short get_env_port(const char* name, unsigned short default_port);

/*
    简单处理 SQL 字符串中的单引号和反斜线。

    这个函数来自第三期 CloudDiskServer.cc。
    正式项目应该使用参数化 SQL；当前课程项目为了降低学习成本，
    继续使用字符串拼接 SQL，所以至少要处理这两个最容易破坏 SQL 字符串的字符。
*/
std::string escape_sql(const std::string& s);

/*
    给 protobuf 响应中的 CommonResult 赋值。

    所有 RPC 响应都带 result 字段，因此用这个小函数统一写入 code/message。
*/
void set_result(cloud::disk::CommonResult* result, int code, const std::string& message);

/*
    执行一条 MySQL SQL，并在回调中处理 MySQLResultCursor。

    为什么这里做成同步等待？
    - srpc 服务端方法是普通虚函数，教学上最容易理解的是“收到请求 -> 查库 -> 填响应”。
    - Workflow 的 MySQL 任务本身仍然是异步执行的；这里只是在服务方法里用 WaitGroup
      等它完成，避免引入更复杂的 RPC 异步响应生命周期。
    - API Gateway 调用 srpc 时仍然使用 create_*_task()，不会阻塞 HTTP 网关线程。

    参数说明：
    - sql：要执行的 SQL。
    - handler：MySQL 成功返回后，用 cursor 读取结果的处理函数。

    返回值：
    - true：网络层和 MySQL 返回包没有失败，handler 已执行。
    - false：MySQL 任务失败或 MySQL 返回错误包，handler 不会执行。
*/
bool run_mysql_query(const std::string& sql,
                     const std::function<void(protocol::MySQLResultCursor&)>& handler);
